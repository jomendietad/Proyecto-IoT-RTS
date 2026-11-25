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
#include "iot_common_types.h"
#include "ble_protocol.h"
#include "status_led.h"
#include "str_metrics.h"
#include "temp_monitor.h"

static const char *TAG = "nodo3_clima";

#define N3_RGB_GPIO            GPIO_NUM_8
#define N3_BOOT_GPIO           GPIO_NUM_9
#define DHT11_PERIOD_MS        3000U
#define DHT11_JITTER_DEAD_US   50000U
#define TEMP_EVAL_DEADLINE_US  50000U

typedef enum {
    CLIMA_MSG_TEMP_UPDATE = 0,
    CLIMA_MSG_SET_THRESHOLD,
} clima_msg_type_t;

typedef struct {
    clima_msg_type_t type;
    int64_t ts_us;
    int16_t temp_deci_c;
    uint8_t hum_pct;
    uint8_t param;
    bool sensor_ok;
} clima_msg_t;

static str_metrics_ctx_t s_metrics;
static QueueHandle_t s_clima_queue = NULL;
static TaskHandle_t s_h_alert = NULL;
static TaskHandle_t s_h_dht = NULL;
static TaskHandle_t s_h_hb = NULL;
static TaskHandle_t s_h_console = NULL;
static uint8_t s_seq = 0;

static int16_t s_last_temp_deci = 240;
static uint8_t s_last_hum_pct = 50;
static bool s_last_sensor_ok = false;

static void task_temp_alert(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);

    clima_msg_t msg;
    while (1) {
        if (xQueueReceive(s_clima_queue, &msg, pdMS_TO_TICKS(2000)) == pdTRUE) {
            if (msg.type == CLIMA_MSG_TEMP_UPDATE) {
                s_last_temp_deci = msg.temp_deci_c;
                s_last_hum_pct = msg.hum_pct;
                s_last_sensor_ok = msg.sensor_ok;

                temp_eval_result_t eval = { 0 };
                temp_monitor_evaluate(msg.temp_deci_c, &eval);

                const uint32_t lat_us = (uint32_t)(esp_timer_get_time() - msg.ts_us);
                if (eval.should_notify) {
                    str_metrics_record(&s_metrics, "temp_alert_eval_us", lat_us, TEMP_EVAL_DEADLINE_US, false);
                }

                if (eval.alert_state != TEMP_ALERT_NORMAL) {
                    status_led_set_state(STATUS_LED_ALARM_TRIGGERED);
                } else {
                    status_led_set_state(STATUS_LED_DISCONNECTED);
                }

                if (eval.should_notify) {
                    const ble_clima_telemetry_t pkt = {
                        .seq_num = ++s_seq,
                        .temp_deci_c = eval.temp_deci_c,
                        .delta_deci_c = eval.delta_deci_c,
                        .humidity_pct = s_last_hum_pct,
                        .alert_state = (uint8_t)eval.alert_state,
                        .threshold_high_c = eval.threshold_high_c,
                    };
                    ESP_LOGW(TAG,
                             "ALERTA TERMICA [%s] T=%.1fC (dT=%+.1fC) H=%u%% T_high=%uC reason=%u | GATT Notify: OP_CLIMA_TELEMETRY seq=%u",
                             iot_temp_alert_str(eval.alert_state),
                             pkt.temp_deci_c / 10.0,
                             pkt.delta_deci_c / 10.0,
                             pkt.humidity_pct,
                             pkt.threshold_high_c,
                             (unsigned int)eval.reason,
                             pkt.seq_num);
                }
            } else if (msg.type == CLIMA_MSG_SET_THRESHOLD) {
                if (temp_monitor_set_threshold(msg.param) == ESP_OK) {
                    ESP_LOGI(TAG, "Umbral superior actualizado y persistido en NVS: T_high=%u C", msg.param);
                }
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
            const clima_msg_t msg = {
                .type = CLIMA_MSG_TEMP_UPDATE,
                .ts_us = esp_timer_get_time(),
                .temp_deci_c = temp_deci,
                .hum_pct = hum,
                .sensor_ok = true,
            };
            xQueueSend(s_clima_queue, &msg, 0);
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
        const TaskHandle_t handles[] = { s_h_alert, s_h_dht, s_h_hb, s_h_console };
        const char *const names[] = { "task_temp_alert", "task_dht11_rmt", "task_hb_n3", "task_console_n3" };
        str_metrics_log_health(&s_metrics, handles, names, 4);

        for (int i = 0; i < 3; i++) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

static void handle_console_line(const char *line)
{
    if (strncmp(line, "umbral ", 7) == 0) {
        const int val = atoi(line + 7);
        if (val >= (int)IOT_TEMP_THRESH_MIN_C && val <= (int)IOT_TEMP_THRESH_MAX_C) {
            const clima_msg_t msg = {
                .type = CLIMA_MSG_SET_THRESHOLD,
                .ts_us = esp_timer_get_time(),
                .param = (uint8_t)val,
            };
            xQueueSend(s_clima_queue, &msg, 0);
        } else {
            ESP_LOGW(TAG, "Rango valido de umbral: %u..%u C",
                     IOT_TEMP_THRESH_MIN_C, IOT_TEMP_THRESH_MAX_C);
        }
    } else if (strncmp(line, "sim_temp ", 9) == 0) {
        const float tc = strtof(line + 9, NULL);
        const clima_msg_t msg = {
            .type = CLIMA_MSG_TEMP_UPDATE,
            .ts_us = esp_timer_get_time(),
            .temp_deci_c = (int16_t)(tc * 10.0f),
            .hum_pct = 55,
            .sensor_ok = true,
        };
        xQueueSend(s_clima_queue, &msg, 0);
        ESP_LOGI(TAG, "Temperatura simulada: %.1f C", (double)tc);
    } else if (strcmp(line, "status") == 0) {
        iot_temp_alert_t alert = TEMP_ALERT_NORMAL;
        uint8_t t_high = IOT_TEMP_THRESH_DEFAULT_C;
        int16_t last_t = 0;
        int16_t last_d = 0;
        temp_monitor_get_state(&alert, &t_high, &last_t, &last_d);
        ESP_LOGI(TAG, "Estado N3: T=%.1fC (dT=%+.1fC) H=%u%% sensor=%s alerta=%s T_high=%uC",
                 s_last_temp_deci / 10.0, last_d / 10.0, s_last_hum_pct,
                 s_last_sensor_ok ? "OK" : "SIN_LECTURA",
                 iot_temp_alert_str(alert), t_high);
    } else if (strcmp(line, "stats") == 0) {
        const TaskHandle_t handles[] = { s_h_alert, s_h_dht, s_h_hb, s_h_console };
        const char *const names[] = { "task_temp_alert", "task_dht11_rmt", "task_hb_n3", "task_console_n3" };
        str_metrics_log_health(&s_metrics, handles, names, 4);
    } else if (line[0] != '\0') {
        ESP_LOGI(TAG, "Comandos: umbral <C> | sim_temp <C> | status | stats");
    }
}

static void task_console_n3(void *arg)
{
    (void)arg;
    char buf[64];
    size_t idx = 0;

    ESP_LOGI(TAG, "Consola N3 lista. Comandos: umbral <C>, sim_temp <C>, status, stats");
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
    status_led_set_state(STATUS_LED_DISCONNECTED);
    ESP_ERROR_CHECK(boot_button_init(N3_BOOT_GPIO, NULL));

    ESP_ERROR_CHECK(temp_monitor_init());
    ESP_ERROR_CHECK(dht11_rmt_init());

    s_clima_queue = xQueueCreate(8, sizeof(clima_msg_t));
    configASSERT(s_clima_queue != NULL);

    xTaskCreate(task_temp_alert, "task_temp_alert", 3072, NULL, 11, &s_h_alert);
    xTaskCreate(task_dht11_rmt, "task_dht11_rmt", 3072, NULL, 6, &s_h_dht);
    xTaskCreate(task_hb_metrics_n3, "task_hb_n3", 3072, NULL, 3, &s_h_hb);
    xTaskCreate(task_console_n3, "task_console_n3", 4096, NULL, 2, &s_h_console);
}
