#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "alarm_fsm.h"
#include "boot_button.h"
#include "iot_common_types.h"
#include "pir_sensor.h"
#include "status_led.h"
#include "str_metrics.h"

static const char *TAG = "nodo2_seguridad";

#define N2_RGB_GPIO  GPIO_NUM_8
#define N2_BOOT_GPIO GPIO_NUM_9

static str_metrics_ctx_t s_metrics;
static TaskHandle_t s_h_alarm = NULL;
static TaskHandle_t s_h_hb = NULL;
static TaskHandle_t s_h_console = NULL;

static void task_hb_metrics_n2(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);

    while (1) {
        const TaskHandle_t handles[] = { s_h_alarm, s_h_hb, s_h_console };
        const char *const names[] = { "task_alarm_fsm", "task_hb_n2", "task_console_n2" };
        str_metrics_log_health(&s_metrics, handles, names, 3);

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
        ESP_LOGI(TAG, "Estado N2: alarma=%s fail_counter=%u pir_pin=%d",
                 iot_alarm_state_str(alarm_fsm_get_state()),
                 alarm_fsm_get_fail_counter(),
                 pir_sensor_is_active() ? 1 : 0);
    } else if (strcmp(line, "stats") == 0) {
        const TaskHandle_t handles[] = { s_h_alarm, s_h_hb, s_h_console };
        const char *const names[] = { "task_alarm_fsm", "task_hb_n2", "task_console_n2" };
        str_metrics_log_health(&s_metrics, handles, names, 3);
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
    status_led_set_state(STATUS_LED_UNPROVISIONED);
    ESP_ERROR_CHECK(boot_button_init(N2_BOOT_GPIO, NULL));

    ESP_ERROR_CHECK(alarm_fsm_init(&s_metrics, &s_h_alarm));

    xTaskCreate(task_hb_metrics_n2, "task_hb_n2", 3072, NULL, 3, &s_h_hb);
    xTaskCreate(task_console_n2, "task_console_n2", 4096, NULL, 2, &s_h_console);
}
