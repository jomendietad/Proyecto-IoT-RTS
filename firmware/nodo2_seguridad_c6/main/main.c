#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "alarm_fsm.h"
#include "ble_peripheral.h"
#include "ble_protocol.h"
#include "boot_button.h"
#include "iot_common_types.h"
#include "pir_sensor.h"
#include "status_led.h"
#include "str_metrics.h"

static const char *TAG = "nodo2_seguridad";

#define N2_RGB_GPIO  GPIO_NUM_8
#define N2_BOOT_GPIO GPIO_NUM_9

typedef struct {
    uint8_t opcode;
    uint8_t len;
    uint8_t payload[BLE_PROTO_MAX_FRAME_LEN];
} n2_ble_cmd_t;

static str_metrics_ctx_t s_metrics;
static ble_dedup_table_t s_dedup = { 0 };
static QueueHandle_t s_ble_cmd_queue = NULL;
static TaskHandle_t s_h_alarm = NULL;
static TaskHandle_t s_h_ble = NULL;
static TaskHandle_t s_h_hb = NULL;
static TaskHandle_t s_h_console = NULL;

static void send_metrics_report_n2(void)
{
    uint32_t max_lat = 0;
    uint32_t avg_jit = 0;
    uint16_t misses = 0;
    str_metrics_snapshot(&s_metrics, &max_lat, &avg_jit, &misses);

    const TaskHandle_t handles[] = { s_h_alarm, s_h_ble, s_h_hb, s_h_console };
    uint16_t min_stack = 0xFFFFU;
    for (size_t i = 0; i < 4U; i++) {
        if (handles[i] != NULL) {
            const uint16_t bytes = (uint16_t)(uxTaskGetStackHighWaterMark(handles[i]) * sizeof(StackType_t));
            if (bytes < min_stack) {
                min_stack = bytes;
            }
        }
    }

    const ble_metrics_report_t rep = {
        .node_role = NODE_ROLE_SEGURIDAD,
        .max_crit_latency_us = max_lat,
        .avg_jitter_us = avg_jit,
        .deadline_misses = misses,
        .min_stack_free_bytes = (min_stack == 0xFFFFU) ? 0U : min_stack,
        .cpu_busy_pct = 0U,
        .last_rx_hops = 0U,
    };
    (void)ble_peripheral_notify(OP_METRICS_REPORT, &rep, sizeof(rep));
}

static void on_ble_cmd(uint8_t opcode, const uint8_t *payload, size_t len, void *ctx)
{
    (void)ctx;
    if (!s_ble_cmd_queue || len > BLE_PROTO_MAX_FRAME_LEN) {
        return;
    }

    n2_ble_cmd_t cmd = {
        .opcode = opcode,
        .len = (uint8_t)len,
    };
    if (len > 0U && payload) {
        memcpy(cmd.payload, payload, len);
    }
    if (xQueueSend(s_ble_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Cola BLE N2 llena (opcode=0x%02X)", opcode);
    }
}

static void on_ble_conn(bool connected, bool subscribed, void *ctx)
{
    (void)ctx;
    if (alarm_fsm_get_state() == ALARM_STATE_DISPARADA) {
        return;
    }
    if (subscribed) {
        status_led_set_state(STATUS_LED_CONNECTED);
    } else if (connected) {
        status_led_set_state(STATUS_LED_CONNECTING);
    } else {
        status_led_set_state(STATUS_LED_DISCONNECTED);
    }
}

static void task_ble_n2(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);

    n2_ble_cmd_t cmd;
    while (1) {
        if (xQueueReceive(s_ble_cmd_queue, &cmd, pdMS_TO_TICKS(2000)) == pdTRUE) {
            if (cmd.opcode == OP_ALARM_SET && cmd.len == sizeof(ble_alarm_set_t)) {
                const ble_alarm_set_t *pkt = (const ble_alarm_set_t *)cmd.payload;
                if (ble_dedup_check(&s_dedup, BLE_NODE_ID_GATEWAY_S3, OP_ALARM_SET, pkt->seq_num)) {
                    const bool from_rfid = (pkt->source == BLE_SRC_RFID_NODO1);
                    if (pkt->target_state == ALARM_STATE_ARMADA) {
                        alarm_fsm_post_event(from_rfid ? ALARM_EVT_RFID_ARM : ALARM_EVT_CMD_ARM);
                    } else {
                        alarm_fsm_post_event(from_rfid ? ALARM_EVT_RFID_DISARM : ALARM_EVT_CMD_DISARM);
                    }
                    ESP_LOGI(TAG, "CMD BLE OP_ALARM_SET target=%s src=%u seq=%u",
                             iot_alarm_state_str((iot_alarm_state_t)pkt->target_state),
                             pkt->source, pkt->seq_num);
                }
            } else if (cmd.opcode == OP_RFID_AUTH_FAIL && cmd.len == sizeof(ble_rfid_auth_fail_t)) {
                const ble_rfid_auth_fail_t *pkt = (const ble_rfid_auth_fail_t *)cmd.payload;
                if (ble_dedup_check(&s_dedup, BLE_NODE_ID_GATEWAY_S3, OP_RFID_AUTH_FAIL, pkt->seq_num)) {
                    alarm_fsm_post_event(ALARM_EVT_RFID_FAIL);
                    ESP_LOGW(TAG, "CMD BLE OP_RFID_AUTH_FAIL seq=%u", pkt->seq_num);
                }
            } else if (cmd.opcode == OP_STATUS_REQ && cmd.len == sizeof(ble_status_req_t)) {
                const ble_status_req_t *req = (const ble_status_req_t *)cmd.payload;
                if (req->req_flags & 0x01U) {
                    alarm_fsm_send_current_status();
                }
                if (req->req_flags & 0x02U) {
                    send_metrics_report_n2();
                }
            } else {
                ESP_LOGW(TAG, "Comando BLE desconocido o invalido: opcode=0x%02X len=%u",
                         cmd.opcode, cmd.len);
            }
        }
        esp_task_wdt_reset();
    }
}

static void task_hb_metrics_n2(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);

    while (1) {
        const TaskHandle_t handles[] = { s_h_alarm, s_h_ble, s_h_hb, s_h_console };
        const char *const names[] = { "task_alarm_fsm", "task_ble_n2", "task_hb_n2", "task_console_n2" };
        str_metrics_log_health(&s_metrics, handles, names, 4);

        uint32_t max_lat = 0;
        uint32_t avg_jit = 0;
        uint16_t misses = 0;
        str_metrics_snapshot(&s_metrics, &max_lat, &avg_jit, &misses);

        const ble_heartbeat_t hb = {
            .node_role = NODE_ROLE_SEGURIDAD,
            .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL),
            .free_heap_kb = (uint16_t)(esp_get_free_heap_size() / 1024U),
            .deadline_misses = misses,
            .flags = (uint8_t)((ble_peripheral_is_subscribed() ? 0x01U : 0x00U) | 0x02U),
        };
        (void)ble_peripheral_notify(OP_HEARTBEAT, &hb, sizeof(hb));

        for (int i = 0; i < 3; i++) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

static void handle_console_line(const char *line)
{
    if (strcmp(line, "arm") == 0) {
        alarm_fsm_post_event(ALARM_EVT_CMD_ARM);
    } else if (strcmp(line, "disarm") == 0) {
        alarm_fsm_post_event(ALARM_EVT_CMD_DISARM);
    } else if (strcmp(line, "sim_pir") == 0) {
        alarm_fsm_trigger_sim_pir();
    } else if (strcmp(line, "sim_fail") == 0) {
        alarm_fsm_post_event(ALARM_EVT_RFID_FAIL);
    } else if (strcmp(line, "status") == 0) {
        ESP_LOGI(TAG, "Estado N2: alarma=%s fail_counter=%u pir_pin=%d ble=%s",
                 iot_alarm_state_str(alarm_fsm_get_state()),
                 alarm_fsm_get_fail_counter(),
                 pir_sensor_is_active() ? 1 : 0,
                 ble_peripheral_is_subscribed() ? "CONECTADO" : "DESCONECTADO");
    } else if (strcmp(line, "stats") == 0) {
        const TaskHandle_t handles[] = { s_h_alarm, s_h_ble, s_h_hb, s_h_console };
        const char *const names[] = { "task_alarm_fsm", "task_ble_n2", "task_hb_n2", "task_console_n2" };
        str_metrics_log_health(&s_metrics, handles, names, 4);
    } else if (line[0] != '\0') {
        ESP_LOGI(TAG, "Comandos: arm | disarm | sim_pir | sim_fail | status | stats");
    }
}

static void task_console_n2(void *arg)
{
    (void)arg;
    char buf[64];
    size_t idx = 0;

    ESP_LOGI(TAG, "Consola N2 lista. Comandos: arm, disarm, sim_pir, sim_fail, status, stats");
    while (1) {
        int ch = getchar();
        if (ch != EOF) {
            if (ch == '\r' || ch == '\n') {
                buf[idx] = '\0';
                if (idx > 0) {
                    handle_console_line(buf);
                    idx = 0;
                }
            } else if (idx + 1 < sizeof(buf)) {
                buf[idx++] = (char)ch;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    str_twdt_init(IOT_TWDT_TIMEOUT_MS);
    str_metrics_init(&s_metrics, "N2");

    ESP_ERROR_CHECK(status_led_init(N2_RGB_GPIO));
    status_led_set_state(STATUS_LED_DISCONNECTED);
    ESP_ERROR_CHECK(boot_button_init(N2_BOOT_GPIO, NULL));

    ESP_ERROR_CHECK(alarm_fsm_init(&s_metrics, &s_h_alarm));

    s_ble_cmd_queue = xQueueCreate(8, sizeof(n2_ble_cmd_t));
    configASSERT(s_ble_cmd_queue != NULL);

    const ble_peripheral_cfg_t ble_cfg = {
        .device_name = BLE_ADV_NAME_NODO2,
        .on_cmd = on_ble_cmd,
        .on_conn = on_ble_conn,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(ble_peripheral_init(&ble_cfg));

    xTaskCreate(task_ble_n2, "task_ble_n2", 4096, NULL, 11, &s_h_ble);
    xTaskCreate(task_hb_metrics_n2, "task_hb_n2", 3072, NULL, 3, &s_h_hb);
    xTaskCreate(task_console_n2, "task_console_n2", 4096, NULL, 2, &s_h_console);
}
