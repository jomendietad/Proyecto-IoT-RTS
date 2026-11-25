#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "boot_button.h"
#include "dht11_rmt.h"
#include "fan_pwm.h"
#include "iot_common_types.h"
#include "mesh_protocol.h"
#include "status_led.h"
#include "str_metrics.h"

static const char *TAG = "nodo3_clima";

#define N3_RGB_GPIO          GPIO_NUM_8
#define N3_BOOT_GPIO         GPIO_NUM_9
#define DHT11_PERIOD_MS      3000U
#define DHT11_JITTER_DEAD_US 50000U
#define FAN_CTRL_DEADLINE_US 50000U

typedef enum {
    FAN_MSG_TEMP_UPDATE = 0,
    FAN_MSG_SET_MANUAL,
    FAN_MSG_SET_AUTO,
    FAN_MSG_SET_THRESHOLD,
} fan_msg_type_t;

typedef struct {
    fan_msg_type_t type;
    int64_t ts_us;
    int16_t temp_deci_c;
    uint8_t hum_pct;
    uint8_t param;
    bool sensor_ok;
} fan_msg_t;

static str_metrics_ctx_t s_metrics;
static QueueHandle_t s_fan_queue = NULL;
static TaskHandle_t s_h_fan = NULL;
static TaskHandle_t s_h_dht = NULL;
static TaskHandle_t s_h_hb = NULL;
static TaskHandle_t s_h_console = NULL;
static uint8_t s_seq = 0;

static int16_t s_last_temp_deci = 240;
static uint8_t s_last_hum_pct = 50;
static bool s_last_sensor_ok = false;

static void task_fan_ctrl(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);

    fan_msg_t msg;
    while (1) {
        if (xQueueReceive(s_fan_queue, &msg, pdMS_TO_TICKS(2000)) == pdTRUE) {
            bool notify_mesh = false;
            mesh_clima_reason_t reason = MESH_CLIMA_REASON_PERIODIC;

            if (msg.type == FAN_MSG_TEMP_UPDATE) {
                s_last_temp_deci = msg.temp_deci_c;
                s_last_hum_pct = msg.hum_pct;
                s_last_sensor_ok = msg.sensor_ok;

                if (fan_pwm_update_from_temp(msg.temp_deci_c)) {
                    const uint32_t lat_us = (uint32_t)(esp_timer_get_time() - msg.ts_us);
                    str_metrics_record(&s_metrics, "fan_pwm_apply_us", lat_us, FAN_CTRL_DEADLINE_US, false);
                    notify_mesh = true;
                    reason = MESH_CLIMA_REASON_LEVEL_CHANGED;
                }
            } else if (msg.type == FAN_MSG_SET_MANUAL) {
                fan_pwm_set_manual((iot_fan_level_t)msg.param);
                const uint32_t lat_us = (uint32_t)(esp_timer_get_time() - msg.ts_us);
                str_metrics_record(&s_metrics, "fan_pwm_apply_us", lat_us, FAN_CTRL_DEADLINE_US, false);
                notify_mesh = true;
                reason = MESH_CLIMA_REASON_LEVEL_CHANGED;
            } else if (msg.type == FAN_MSG_SET_AUTO) {
                fan_pwm_set_auto();
                const uint32_t lat_us = (uint32_t)(esp_timer_get_time() - msg.ts_us);
                str_metrics_record(&s_metrics, "fan_pwm_apply_us", lat_us, FAN_CTRL_DEADLINE_US, false);
                notify_mesh = true;
                reason = MESH_CLIMA_REASON_LEVEL_CHANGED;
            } else if (msg.type == FAN_MSG_SET_THRESHOLD) {
                if (fan_pwm_set_threshold(msg.param) == ESP_OK) {
                    notify_mesh = true;
                    reason = MESH_CLIMA_REASON_LEVEL_CHANGED;
                }
            }

            if (notify_mesh) {
                iot_fan_mode_t mode = FAN_MODE_AUTO;
                iot_fan_level_t lvl = FAN_LEVEL_OFF;
                uint8_t t_base = IOT_FAN_TBASE_DEFAULT_C;
                fan_pwm_get_state(&mode, &lvl, &t_base);

                const mesh_clima_telemetry_t pkt = {
                    .seq_num = ++s_seq,
                    .temp_deci_c = s_last_temp_deci,
                    .humidity_pct = s_last_hum_pct,
                    .fan_mode = (uint8_t)mode,
                    .fan_level = (uint8_t)lvl,
                    .threshold_base_c = t_base,
                    .change_reason = (uint8_t)reason,
                };
                ESP_LOGI(TAG, "Clima T=%.1fC H=%u%% Modo=%s Nivel=%s Tbase=%uC | Mesh 0xC003: OP_CLIMA_TELEMETRY seq=%u",
                         pkt.temp_deci_c / 10.0, pkt.humidity_pct,
                         iot_fan_mode_str(mode), iot_fan_level_str(lvl),
                         pkt.threshold_base_c, pkt.seq_num);
            }
        }
        esp_task_wdt_reset();
    }
}

static void task_dht11_rmt(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);

    TickType_t last_wake = xTaskGetTickCount();
    int64_t expected_us = esp_timer_get_time() + ((int64_t)DHT11_PERIOD_MS * 1000LL);
    uint8_t err_streak = 0;

    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(DHT11_PERIOD_MS));

        const int64_t now_us = esp_timer_get_time();
        const int64_t diff = now_us - expected_us;
        const uint32_t jitter_us = (uint32_t)((diff >= 0) ? diff : -diff);
        expected_us += ((int64_t)DHT11_PERIOD_MS * 1000LL);

        str_metrics_record(&s_metrics, "dht11_jitter_us", jitter_us, DHT11_JITTER_DEAD_US, true);

        int16_t temp_deci = 0;
        uint8_t hum = 0;
        const esp_err_t err = dht11_rmt_read(&temp_deci, &hum);
        if (err == ESP_OK) {
            err_streak = 0;
            const fan_msg_t msg = {
                .type = FAN_MSG_TEMP_UPDATE,
                .ts_us = esp_timer_get_time(),
                .temp_deci_c = temp_deci,
                .hum_pct = hum,
                .sensor_ok = true,
            };
            xQueueSend(s_fan_queue, &msg, 0);
        } else {
            err_streak++;
            if (err_streak == 1U || (err_streak % 5U) == 0U) {
                ESP_LOGW(TAG, "Lectura DHT11 RMT fallida (%s, racha=%u); usa 'sim_temp <C>' si no hay sensor",
                         esp_err_to_name(err), err_streak);
            }
        }

        esp_task_wdt_reset();
    }
}

static void task_hb_metrics_n3(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);

    while (1) {
        const TaskHandle_t handles[] = { s_h_fan, s_h_dht, s_h_hb, s_h_console };
        const char *const names[] = { "task_fan_ctrl", "task_dht11_rmt", "task_hb_n3", "task_console_n3" };
        str_metrics_log_health(&s_metrics, handles, names, 4);

        for (int i = 0; i < 3; i++) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

static void handle_console_line(const char *line)
{
    if (strcmp(line, "fan auto") == 0) {
        const fan_msg_t msg = { .type = FAN_MSG_SET_AUTO, .ts_us = esp_timer_get_time() };
        xQueueSend(s_fan_queue, &msg, 0);
    } else if (strcmp(line, "fan off") == 0) {
        const fan_msg_t msg = { .type = FAN_MSG_SET_MANUAL, .ts_us = esp_timer_get_time(), .param = FAN_LEVEL_OFF };
        xQueueSend(s_fan_queue, &msg, 0);
    } else if (strcmp(line, "fan baja") == 0) {
        const fan_msg_t msg = { .type = FAN_MSG_SET_MANUAL, .ts_us = esp_timer_get_time(), .param = FAN_LEVEL_BAJA };
        xQueueSend(s_fan_queue, &msg, 0);
    } else if (strcmp(line, "fan media") == 0) {
        const fan_msg_t msg = { .type = FAN_MSG_SET_MANUAL, .ts_us = esp_timer_get_time(), .param = FAN_LEVEL_MEDIA };
        xQueueSend(s_fan_queue, &msg, 0);
    } else if (strcmp(line, "fan alta") == 0) {
        const fan_msg_t msg = { .type = FAN_MSG_SET_MANUAL, .ts_us = esp_timer_get_time(), .param = FAN_LEVEL_ALTA };
        xQueueSend(s_fan_queue, &msg, 0);
    } else if (strncmp(line, "umbral ", 7) == 0) {
        const int val = atoi(line + 7);
        if (val >= (int)IOT_FAN_TBASE_MIN_C && val <= (int)IOT_FAN_TBASE_MAX_C) {
            const fan_msg_t msg = { .type = FAN_MSG_SET_THRESHOLD, .ts_us = esp_timer_get_time(), .param = (uint8_t)val };
            xQueueSend(s_fan_queue, &msg, 0);
        } else {
            ESP_LOGW(TAG, "Rango valido de umbral: %u..%u C", IOT_FAN_TBASE_MIN_C, IOT_FAN_TBASE_MAX_C);
        }
    } else if (strncmp(line, "sim_temp ", 9) == 0) {
        const float tc = strtof(line + 9, NULL);
        const fan_msg_t msg = {
            .type = FAN_MSG_TEMP_UPDATE,
            .ts_us = esp_timer_get_time(),
            .temp_deci_c = (int16_t)(tc * 10.0f),
            .hum_pct = 55,
            .sensor_ok = true,
        };
        xQueueSend(s_fan_queue, &msg, 0);
        ESP_LOGI(TAG, "Temperatura simulada: %.1f C", (double)tc);
    } else if (strcmp(line, "status") == 0) {
        iot_fan_mode_t mode = FAN_MODE_AUTO;
        iot_fan_level_t lvl = FAN_LEVEL_OFF;
        uint8_t t_base = IOT_FAN_TBASE_DEFAULT_C;
        fan_pwm_get_state(&mode, &lvl, &t_base);
        ESP_LOGI(TAG, "Estado N3: T=%.1fC H=%u%% sensor=%s modo=%s nivel=%s T_base=%uC",
                 s_last_temp_deci / 10.0, s_last_hum_pct,
                 s_last_sensor_ok ? "OK" : "SIN_LECTURA",
                 iot_fan_mode_str(mode), iot_fan_level_str(lvl), t_base);
    } else if (strcmp(line, "stats") == 0) {
        const TaskHandle_t handles[] = { s_h_fan, s_h_dht, s_h_hb, s_h_console };
        const char *const names[] = { "task_fan_ctrl", "task_dht11_rmt", "task_hb_n3", "task_console_n3" };
        str_metrics_log_health(&s_metrics, handles, names, 4);
    } else if (line[0] != '\0') {
        ESP_LOGI(TAG, "Comandos: fan auto|off|baja|media|alta | umbral <C> | sim_temp <C> | status | stats");
    }
}

static void task_console_n3(void *arg)
{
    (void)arg;
    char buf[64];
    size_t idx = 0;

    ESP_LOGI(TAG, "Consola N3 lista. Comandos: fan auto|off|baja|media|alta, umbral <C>, sim_temp <C>, status, stats");
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
    str_metrics_init(&s_metrics, "N3");

    ESP_ERROR_CHECK(status_led_init(N3_RGB_GPIO));
    status_led_set_state(STATUS_LED_UNPROVISIONED);
    ESP_ERROR_CHECK(boot_button_init(N3_BOOT_GPIO, NULL));

    ESP_ERROR_CHECK(fan_pwm_init());
    ESP_ERROR_CHECK(dht11_rmt_init());

    s_fan_queue = xQueueCreate(8, sizeof(fan_msg_t));
    configASSERT(s_fan_queue != NULL);

    xTaskCreate(task_fan_ctrl, "task_fan_ctrl", 3072, NULL, 11, &s_h_fan);
    xTaskCreate(task_dht11_rmt, "task_dht11_rmt", 3072, NULL, 6, &s_h_dht);
    xTaskCreate(task_hb_metrics_n3, "task_hb_n3", 3072, NULL, 3, &s_h_hb);
    xTaskCreate(task_console_n3, "task_console_n3", 4096, NULL, 2, &s_h_console);
}
