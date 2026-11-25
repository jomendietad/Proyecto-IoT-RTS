#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "ble_central_star.h"
#include "ble_protocol.h"
#include "boot_button.h"
#include "iot_common_types.h"
#include "sdkconfig.h"
#include "star_coordinator.h"
#include "status_led.h"
#include "str_metrics.h"

static const char *TAG = "gateway_s3";

static str_metrics_ctx_t s_metrics;
static TaskHandle_t s_h_fsm = NULL;
static TaskHandle_t s_h_hb = NULL;
static TaskHandle_t s_h_console = NULL;

static bool parse_uid_hex(const char *hex, uint8_t *out_uid, uint8_t *out_len)
{
    if (!hex || !out_uid || !out_len) {
        return false;
    }

    uint8_t buf[IOT_RFID_UID_MAX_LEN] = { 0 };
    uint8_t idx = 0;
    const char *p = hex;

    while (*p != '\0') {
        while (*p == ' ' || *p == ':' || *p == '-') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1]) || idx >= IOT_RFID_UID_MAX_LEN) {
            return false;
        }
        char byte_str[3] = { p[0], p[1], '\0' };
        buf[idx++] = (uint8_t)strtoul(byte_str, NULL, 16);
        p += 2;
    }

    if (idx != 4U && idx != 7U) {
        return false;
    }
    memcpy(out_uid, buf, idx);
    *out_len = idx;
    return true;
}

static void task_hb_metrics_s3(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);

    while (1) {
        const TaskHandle_t handles[] = { s_h_fsm, s_h_hb, s_h_console };
        const char *const names[] = { "task_state_fsm_s3", "task_hb_s3", "task_console_s3" };
        str_metrics_log_health(&s_metrics, handles, names, 3);

        for (int i = 0; i < 3; i++) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

static void handle_console_line(const char *line)
{
    if (strcmp(line, "status") == 0 || strcmp(line, "estado") == 0) {
        star_cmd_request_status_all(0x01U);
        star_log_global_status();
    } else if (strcmp(line, "net") == 0 || strcmp(line, "red") == 0) {
        star_log_ble_network();
    } else if (strcmp(line, "arm") == 0) {
        ESP_LOGI(TAG, "CMD -> /armar (%s)", esp_err_to_name(star_cmd_alarm(ALARM_STATE_ARMADA)));
    } else if (strcmp(line, "disarm") == 0) {
        ESP_LOGI(TAG, "CMD -> /desarmar (%s)", esp_err_to_name(star_cmd_alarm(ALARM_STATE_DESARMADA)));
    } else if (strcmp(line, "lock open") == 0) {
        ESP_LOGI(TAG, "CMD -> /cerradura abrir (%s)", esp_err_to_name(star_cmd_lock(LOCK_STATE_ABIERTA)));
    } else if (strcmp(line, "lock close") == 0) {
        ESP_LOGI(TAG, "CMD -> /cerradura cerrar (%s)", esp_err_to_name(star_cmd_lock(LOCK_STATE_CERRADA)));
    } else if (strcmp(line, "rfid list") == 0) {
        ESP_LOGI(TAG, "CMD -> /rfid list (%s)", esp_err_to_name(star_cmd_rfid(BLE_RFID_ACT_LIST, NULL, 0U)));
    } else if (strncmp(line, "rfid add ", 9) == 0) {
        uint8_t uid[IOT_RFID_UID_MAX_LEN];
        uint8_t len = 0;
        if (!parse_uid_hex(line + 9, uid, &len)) {
            ESP_LOGW(TAG, "UID invalido. Usa 4 o 7 bytes hex (ej: rfid add A1B2C3D4)");
        } else {
            ESP_LOGI(TAG, "CMD -> /rfid add (%s)", esp_err_to_name(star_cmd_rfid(BLE_RFID_ACT_ADD, uid, len)));
        }
    } else if (strncmp(line, "rfid del ", 9) == 0) {
        uint8_t uid[IOT_RFID_UID_MAX_LEN];
        uint8_t len = 0;
        if (!parse_uid_hex(line + 9, uid, &len)) {
            ESP_LOGW(TAG, "UID invalido.");
        } else {
            ESP_LOGI(TAG, "CMD -> /rfid del (%s)", esp_err_to_name(star_cmd_rfid(BLE_RFID_ACT_DEL, uid, len)));
        }
    } else if (strncmp(line, "umbral ", 7) == 0) {
        const int val = atoi(line + 7);
        ESP_LOGI(TAG, "CMD -> /umbral %d (%s)", val, esp_err_to_name(star_cmd_threshold((uint8_t)val)));
    } else if (strcmp(line, "metrics") == 0) {
        star_cmd_request_status_all(0x02U);
        const TaskHandle_t handles[] = { s_h_fsm, s_h_hb, s_h_console };
        const char *const names[] = { "task_state_fsm_s3", "task_hb_s3", "task_console_s3" };
        str_metrics_log_health(&s_metrics, handles, names, 3);
    } else if (strcmp(line, "stats") == 0) {
        const TaskHandle_t handles[] = { s_h_fsm, s_h_hb, s_h_console };
        const char *const names[] = { "task_state_fsm_s3", "task_hb_s3", "task_console_s3" };
        str_metrics_log_health(&s_metrics, handles, names, 3);
    } else if (line[0] != '\0') {
        ESP_LOGI(TAG, "Comandos S3: status | net | arm | disarm | lock open|close | rfid list|add|del | umbral <C> | metrics | stats");
    }
}

static void task_console_s3(void *arg)
{
    (void)arg;
    char buf[96];
    size_t idx = 0;

    ESP_LOGI(TAG, "Consola S3 lista (Core %d). Comandos: status, net, arm, disarm, lock open|close, rfid list|add|del, umbral <C>, metrics",
             xPortGetCoreID());
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
    str_metrics_init(&s_metrics, "S3");

    ESP_ERROR_CHECK(status_led_init((gpio_num_t)CONFIG_GW_RGB_GPIO));
    status_led_set_state(STATUS_LED_DISCONNECTED);
    ESP_ERROR_CHECK(boot_button_init((gpio_num_t)CONFIG_GW_BOOT_GPIO, NULL));

    ESP_ERROR_CHECK(star_coordinator_init(&s_metrics, &s_h_fsm));

    const ble_central_cfg_t ble_cfg = {
        .on_rx = star_coordinator_on_ble_rx,
        .on_node_state = star_coordinator_on_node_state,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(ble_central_star_init(&ble_cfg));

    xTaskCreatePinnedToCore(task_hb_metrics_s3, "task_hb_s3", 3072, NULL, 3, &s_h_hb, 1);
    xTaskCreatePinnedToCore(task_console_s3, "task_console_s3", 4096, NULL, 2, &s_h_console, 1);
}
