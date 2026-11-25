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

#include "ble_peripheral.h"
#include "ble_protocol.h"
#include "boot_button.h"
#include "iot_common_types.h"
#include "lock_ctrl.h"
#include "rc522_spi.h"
#include "rfid_store_nvs.h"
#include "status_led.h"
#include "str_metrics.h"

static const char *TAG = "nodo1_acceso";

#define N1_RGB_GPIO          GPIO_NUM_8
#define N1_BOOT_GPIO         GPIO_NUM_9
#define RFID_POLL_PERIOD_MS  100U
#define RFID_COOLDOWN_US     (1200000LL)
#define RFID_DEADLINE_US     50000U

typedef struct {
    uint8_t opcode;
    uint8_t len;
    uint8_t payload[BLE_PROTO_MAX_FRAME_LEN];
} n1_ble_cmd_t;

static str_metrics_ctx_t s_metrics;
static ble_dedup_table_t s_dedup = { 0 };
static QueueHandle_t s_ble_cmd_queue = NULL;
static TaskHandle_t s_h_rfid = NULL;
static TaskHandle_t s_h_ble = NULL;
static TaskHandle_t s_h_hb = NULL;
static TaskHandle_t s_h_console = NULL;
static uint8_t s_seq = 0;

static void send_lock_status_ble(ble_lock_src_t src, const uint8_t *uid, uint8_t uid_len)
{
    ble_lock_status_t lock_pkt = {
        .seq_num = ++s_seq,
        .lock_state = (uint8_t)lock_ctrl_get(),
        .source = (uint8_t)src,
        .uid_len = uid_len,
    };
    if (uid && uid_len > 0U) {
        const size_t n = (uid_len > IOT_RFID_UID_MAX_LEN) ? IOT_RFID_UID_MAX_LEN : uid_len;
        memcpy(lock_pkt.uid, uid, n);
    }
    (void)ble_peripheral_notify(OP_LOCK_STATUS, &lock_pkt, sizeof(lock_pkt));
}

static void send_metrics_report_n1(void)
{
    uint32_t max_lat = 0;
    uint32_t avg_jit = 0;
    uint16_t misses = 0;
    str_metrics_snapshot(&s_metrics, &max_lat, &avg_jit, &misses);

    const TaskHandle_t handles[] = { s_h_rfid, s_h_ble, s_h_hb, s_h_console };
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
        .node_role = NODE_ROLE_ACCESO,
        .max_crit_latency_us = max_lat,
        .avg_jitter_us = avg_jit,
        .deadline_misses = misses,
        .min_stack_free_bytes = (min_stack == 0xFFFFU) ? 0U : min_stack,
        .cpu_busy_pct = 0U,
        .last_rx_hops = 0U,
    };
    (void)ble_peripheral_notify(OP_METRICS_REPORT, &rep, sizeof(rep));
}

static void process_rfid_event(const uint8_t *uid, uint8_t uid_len, int64_t t_read_us, bool is_sim)
{
    char uid_str[24];
    rfid_store_format_uid(uid, uid_len, uid_str, sizeof(uid_str));

    if (rfid_store_is_authorized(uid, uid_len)) {
        const iot_lock_state_t new_lock = lock_ctrl_toggle();
        const uint32_t lat_us = (uint32_t)(esp_timer_get_time() - t_read_us);
        str_metrics_record(&s_metrics, "rfid_to_lock_us", lat_us, RFID_DEADLINE_US, false);

        send_lock_status_ble(BLE_LOCK_SRC_RFID_VALID, uid, uid_len);

        const iot_alarm_state_t target_alarm =
            (new_lock == LOCK_STATE_ABIERTA) ? ALARM_STATE_DESARMADA : ALARM_STATE_ARMADA;
        const ble_alarm_set_t alarm_pkt = {
            .seq_num = ++s_seq,
            .target_state = (uint8_t)target_alarm,
            .source = BLE_SRC_RFID_NODO1,
        };
        const esp_err_t tx_err = ble_peripheral_notify(OP_ALARM_SET, &alarm_pkt, sizeof(alarm_pkt));

        ESP_LOGI(TAG, "RFID %s AUTORIZADO [%s] -> Cerradura=%s | GATT Notify: OP_ALARM_SET(%s) seq=%u (%s)",
                 is_sim ? "(SIM)" : "(SPI)",
                 uid_str,
                 iot_lock_state_str(new_lock),
                 iot_alarm_state_str(target_alarm),
                 alarm_pkt.seq_num,
                 (tx_err == ESP_OK) ? "TX_OK" : "LOCAL");
    } else {
        ble_rfid_auth_fail_t fail_pkt = {
            .seq_num = ++s_seq,
            .uid_len = uid_len,
            .local_attempt_hint = 1,
        };
        const size_t n = (uid_len > IOT_RFID_UID_MAX_LEN) ? IOT_RFID_UID_MAX_LEN : uid_len;
        memcpy(fail_pkt.uid, uid, n);
        const esp_err_t tx_err = ble_peripheral_notify(OP_RFID_AUTH_FAIL, &fail_pkt, sizeof(fail_pkt));

        ESP_LOGW(TAG, "RFID %s RECHAZADO [%s] -> GATT Notify: OP_RFID_AUTH_FAIL seq=%u (%s)",
                 is_sim ? "(SIM)" : "(SPI)", uid_str, fail_pkt.seq_num,
                 (tx_err == ESP_OK) ? "TX_OK" : "LOCAL");
    }
}

static void on_ble_cmd(uint8_t opcode, const uint8_t *payload, size_t len, void *ctx)
{
    (void)ctx;
    if (!s_ble_cmd_queue || len > BLE_PROTO_MAX_FRAME_LEN) {
        return;
    }

    n1_ble_cmd_t cmd = {
        .opcode = opcode,
        .len = (uint8_t)len,
    };
    if (len > 0U && payload) {
        memcpy(cmd.payload, payload, len);
    }
    if (xQueueSend(s_ble_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Cola de comandos BLE N1 llena (opcode=0x%02X)", opcode);
    }
}

static void on_ble_conn(bool connected, bool subscribed, void *ctx)
{
    (void)ctx;
    if (subscribed) {
        status_led_set_state(STATUS_LED_CONNECTED);
    } else if (connected) {
        status_led_set_state(STATUS_LED_CONNECTING);
    } else {
        status_led_set_state(STATUS_LED_DISCONNECTED);
    }
}

static void handle_ble_rfid_mgmt(const ble_rfid_mgmt_t *req)
{
    uint8_t op_status = 0; /* OK */
    if (req->action == BLE_RFID_ACT_ADD) {
        const esp_err_t err = rfid_store_add(req->uid, req->uid_len);
        if (err == ESP_ERR_NO_MEM) {
            op_status = 2;
        } else if (err != ESP_OK) {
            op_status = 1;
        }
    } else if (req->action == BLE_RFID_ACT_DEL) {
        const esp_err_t err = rfid_store_del(req->uid, req->uid_len);
        if (err == ESP_ERR_NOT_FOUND) {
            op_status = 3;
        } else if (err != ESP_OK) {
            op_status = 1;
        }
    } else if (req->action != BLE_RFID_ACT_LIST) {
        op_status = 1;
    }

    rfid_card_entry_t cards[IOT_RFID_MAX_CARDS];
    size_t count = 0;
    rfid_store_get_all(cards, IOT_RFID_MAX_CARDS, &count);
    if (count > IOT_RFID_MAX_CARDS) {
        count = IOT_RFID_MAX_CARDS;
    }

    ble_rfid_mgmt_ack_t ack = {
        .seq_num = req->seq_num,
        .action = req->action,
        .status = op_status,
        .count = (uint8_t)count,
    };
    for (size_t i = 0; i < count; i++) {
        ack.cards[i].uid_len = cards[i].len;
        const size_t n = (cards[i].len > IOT_RFID_UID_MAX_LEN) ? IOT_RFID_UID_MAX_LEN : cards[i].len;
        memcpy(ack.cards[i].uid, cards[i].uid, n);
    }

    const size_t wire_len = 4U + (count * sizeof(ble_rfid_card_wire_t));
    (void)ble_peripheral_notify(OP_RFID_MGMT_ACK, &ack, wire_len);
    ESP_LOGI(TAG, "CMD BLE OP_RFID_MGMT action=%u status=%u count=%u",
             req->action, op_status, (unsigned int)count);
}

static void task_ble_n1(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);

    n1_ble_cmd_t cmd;
    while (1) {
        if (xQueueReceive(s_ble_cmd_queue, &cmd, pdMS_TO_TICKS(2000)) == pdTRUE) {
            if (cmd.opcode == OP_LOCK_SET && cmd.len == sizeof(ble_lock_set_t)) {
                const ble_lock_set_t *pkt = (const ble_lock_set_t *)cmd.payload;
                if (ble_dedup_check(&s_dedup, BLE_NODE_ID_GATEWAY_S3, OP_LOCK_SET, pkt->seq_num)) {
                    const iot_lock_state_t target = (pkt->lock_cmd != 0U) ? LOCK_STATE_ABIERTA : LOCK_STATE_CERRADA;
                    lock_ctrl_set(target);
                    send_lock_status_ble(BLE_LOCK_SRC_TELEGRAM, NULL, 0U);
                    ESP_LOGI(TAG, "CMD BLE OP_LOCK_SET -> %s (seq=%u)",
                             iot_lock_state_str(target), pkt->seq_num);
                }
            } else if (cmd.opcode == OP_RFID_MGMT && cmd.len == sizeof(ble_rfid_mgmt_t)) {
                const ble_rfid_mgmt_t *pkt = (const ble_rfid_mgmt_t *)cmd.payload;
                if (ble_dedup_check(&s_dedup, BLE_NODE_ID_GATEWAY_S3, OP_RFID_MGMT, pkt->seq_num)) {
                    handle_ble_rfid_mgmt(pkt);
                }
            } else if (cmd.opcode == OP_STATUS_REQ && cmd.len == sizeof(ble_status_req_t)) {
                const ble_status_req_t *req = (const ble_status_req_t *)cmd.payload;
                if (req->req_flags & 0x01U) {
                    send_lock_status_ble(BLE_LOCK_SRC_TELEGRAM, NULL, 0U);
                }
                if (req->req_flags & 0x02U) {
                    send_metrics_report_n1();
                }
            } else {
                ESP_LOGW(TAG, "Comando BLE desconocido o invalido: opcode=0x%02X len=%u",
                         cmd.opcode, cmd.len);
            }
        }
        esp_task_wdt_reset();
    }
}

static void task_rfid_poll(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);

    TickType_t last_wake = xTaskGetTickCount();
    uint8_t last_uid[IOT_RFID_UID_MAX_LEN] = {0};
    uint8_t last_len = 0;
    int64_t last_uid_us = 0;

    while (1) {
        uint8_t uid[IOT_RFID_UID_MAX_LEN] = {0};
        uint8_t uid_len = 0;

        if (rc522_read_uid(uid, &uid_len) == ESP_OK) {
            const int64_t now_us = esp_timer_get_time();
            const bool same_card = (uid_len == last_len) && (memcmp(uid, last_uid, uid_len) == 0);
            if (!same_card || (now_us - last_uid_us) >= RFID_COOLDOWN_US) {
                memcpy(last_uid, uid, uid_len);
                last_len = uid_len;
                last_uid_us = now_us;
                process_rfid_event(uid, uid_len, now_us, false);
            }
        }

        esp_task_wdt_reset();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(RFID_POLL_PERIOD_MS));
    }
}

static void task_hb_metrics_n1(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);

    while (1) {
        const TaskHandle_t handles[] = { s_h_rfid, s_h_ble, s_h_hb, s_h_console };
        const char *const names[] = { "task_rfid_poll", "task_ble_n1", "task_hb_n1", "task_console_n1" };
        str_metrics_log_health(&s_metrics, handles, names, 4);

        uint32_t max_lat = 0;
        uint32_t avg_jit = 0;
        uint16_t misses = 0;
        str_metrics_snapshot(&s_metrics, &max_lat, &avg_jit, &misses);

        const ble_heartbeat_t hb = {
            .node_role = NODE_ROLE_ACCESO,
            .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL),
            .free_heap_kb = (uint16_t)(esp_get_free_heap_size() / 1024U),
            .deadline_misses = misses,
            .flags = (uint8_t)((ble_peripheral_is_subscribed() ? 0x01U : 0x00U) |
                               (rc522_spi_is_present() ? 0x02U : 0x00U)),
        };
        (void)ble_peripheral_notify(OP_HEARTBEAT, &hb, sizeof(hb));

        /* Alimenta TWDT en espera de 15 s dividida en 3 pasos < 10 s */
        for (int i = 0; i < 3; i++) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

static void handle_console_line(char *line)
{
    if (strcmp(line, "open") == 0) {
        lock_ctrl_set(LOCK_STATE_ABIERTA);
        send_lock_status_ble(BLE_LOCK_SRC_TELEGRAM, NULL, 0U);
        ESP_LOGI(TAG, "Comando consola -> Cerradura ABIERTA");
    } else if (strcmp(line, "close") == 0) {
        lock_ctrl_set(LOCK_STATE_CERRADA);
        send_lock_status_ble(BLE_LOCK_SRC_TELEGRAM, NULL, 0U);
        ESP_LOGI(TAG, "Comando consola -> Cerradura CERRADA");
    } else if (strcmp(line, "rfid list") == 0) {
        rfid_card_entry_t cards[IOT_RFID_MAX_CARDS];
        size_t count = 0;
        rfid_store_get_all(cards, IOT_RFID_MAX_CARDS, &count);
        ESP_LOGI(TAG, "Tarjetas autorizadas (%u):", (unsigned int)count);
        for (size_t i = 0; i < count; i++) {
            char buf[24];
            rfid_store_format_uid(cards[i].uid, cards[i].len, buf, sizeof(buf));
            ESP_LOGI(TAG, "  [%u] %s", (unsigned int)i + 1U, buf);
        }
    } else if (strncmp(line, "rfid add ", 9) == 0) {
        uint8_t uid[IOT_RFID_UID_MAX_LEN];
        uint8_t len = 0;
        if (!rfid_store_parse_hex(line + 9, uid, &len)) {
            ESP_LOGW(TAG, "UID invalido. Ejemplo: rfid add A1B2C3D4");
        } else {
            esp_err_t err = rfid_store_add(uid, len);
            ESP_LOGI(TAG, "rfid add -> %s", esp_err_to_name(err));
        }
    } else if (strncmp(line, "rfid del ", 9) == 0) {
        uint8_t uid[IOT_RFID_UID_MAX_LEN];
        uint8_t len = 0;
        if (!rfid_store_parse_hex(line + 9, uid, &len)) {
            ESP_LOGW(TAG, "UID invalido.");
        } else {
            esp_err_t err = rfid_store_del(uid, len);
            ESP_LOGI(TAG, "rfid del -> %s", esp_err_to_name(err));
        }
    } else if (strncmp(line, "sim_ok ", 7) == 0 || strcmp(line, "sim_ok") == 0) {
        uint8_t uid[IOT_RFID_UID_MAX_LEN] = { 0xB6, 0x7B, 0x0C, 0x03 };
        uint8_t len = 4;
        if (strlen(line) > 7) {
            rfid_store_parse_hex(line + 7, uid, &len);
        }
        process_rfid_event(uid, len, esp_timer_get_time(), true);
    } else if (strncmp(line, "sim_fail ", 9) == 0 || strcmp(line, "sim_fail") == 0) {
        uint8_t uid[IOT_RFID_UID_MAX_LEN] = { 0xBA, 0xAD, 0xF0, 0x0D };
        uint8_t len = 4;
        if (strlen(line) > 9) {
            rfid_store_parse_hex(line + 9, uid, &len);
        }
        process_rfid_event(uid, len, esp_timer_get_time(), true);
    } else if (strcmp(line, "status") == 0) {
        ESP_LOGI(TAG, "Estado N1: cerradura=%s rc522=%s ble=%s",
                 iot_lock_state_str(lock_ctrl_get()),
                 rc522_spi_is_present() ? "OK" : "AUSENTE",
                 ble_peripheral_is_subscribed() ? "CONECTADO" : "DESCONECTADO");
    } else if (strcmp(line, "stats") == 0) {
        const TaskHandle_t handles[] = { s_h_rfid, s_h_ble, s_h_hb, s_h_console };
        const char *const names[] = { "task_rfid_poll", "task_ble_n1", "task_hb_n1", "task_console_n1" };
        str_metrics_log_health(&s_metrics, handles, names, 4);
    } else if (line[0] != '\0') {
        ESP_LOGI(TAG, "Comandos: open | close | rfid list | rfid add <HEX> | rfid del <HEX> | sim_ok | sim_fail | status | stats");
    }
}

static void task_console_n1(void *arg)
{
    (void)arg;
    char buf[96];
    size_t idx = 0;

    ESP_LOGI(TAG, "Consola N1 lista. Comandos: open, close, rfid list|add|del, sim_ok, sim_fail, status, stats");
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
    str_metrics_init(&s_metrics, "N1");

    ESP_ERROR_CHECK(status_led_init(N1_RGB_GPIO));
    status_led_set_state(STATUS_LED_DISCONNECTED);
    ESP_ERROR_CHECK(boot_button_init(N1_BOOT_GPIO, NULL));

    ESP_ERROR_CHECK(rfid_store_init());
    ESP_ERROR_CHECK(lock_ctrl_init());
    rc522_spi_init(); /* No fatal si no hay módulo RC522 conectado en banco de pruebas */

    s_ble_cmd_queue = xQueueCreate(8, sizeof(n1_ble_cmd_t));
    configASSERT(s_ble_cmd_queue != NULL);

    const ble_peripheral_cfg_t ble_cfg = {
        .device_name = BLE_ADV_NAME_NODO1,
        .on_cmd = on_ble_cmd,
        .on_conn = on_ble_conn,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(ble_peripheral_init(&ble_cfg));

    xTaskCreate(task_rfid_poll, "task_rfid_poll", 4096, NULL, 12, &s_h_rfid);
    xTaskCreate(task_ble_n1, "task_ble_n1", 4096, NULL, 10, &s_h_ble);
    xTaskCreate(task_hb_metrics_n1, "task_hb_n1", 3072, NULL, 3, &s_h_hb);
    xTaskCreate(task_console_n1, "task_console_n1", 4096, NULL, 2, &s_h_console);
}
