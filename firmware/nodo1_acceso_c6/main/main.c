#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "boot_button.h"
#include "iot_common_types.h"
#include "lock_ctrl.h"
#include "mesh_protocol.h"
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

static str_metrics_ctx_t s_metrics;
static TaskHandle_t s_h_rfid = NULL;
static TaskHandle_t s_h_hb = NULL;
static TaskHandle_t s_h_console = NULL;
static uint8_t s_seq = 0;

static void process_rfid_event(const uint8_t *uid, uint8_t uid_len, int64_t t_read_us, bool is_sim)
{
    char uid_str[24];
    rfid_store_format_uid(uid, uid_len, uid_str, sizeof(uid_str));

    if (rfid_store_is_authorized(uid, uid_len)) {
        const iot_lock_state_t new_lock = lock_ctrl_toggle();
        const uint32_t lat_us = (uint32_t)(esp_timer_get_time() - t_read_us);
        str_metrics_record(&s_metrics, "rfid_to_lock_us", lat_us, RFID_DEADLINE_US, false);

        mesh_lock_status_t lock_pkt = {
            .seq_num = ++s_seq,
            .lock_state = (uint8_t)new_lock,
            .source = MESH_LOCK_SRC_RFID_VALID,
            .uid_len = uid_len,
        };
        memcpy(lock_pkt.uid, uid, uid_len);

        const iot_alarm_state_t target_alarm =
            (new_lock == LOCK_STATE_ABIERTA) ? ALARM_STATE_DESARMADA : ALARM_STATE_ARMADA;
        mesh_alarm_set_t alarm_pkt = {
            .seq_num = ++s_seq,
            .target_state = (uint8_t)target_alarm,
            .source = MESH_SRC_RFID_C6_DIRECT,
        };

        ESP_LOGI(TAG, "RFID %s AUTORIZADO [%s] -> Cerradura=%s | Mesh 0xC001: OP_ALARM_SET(%s) seq=%u",
                 is_sim ? "(SIM)" : "(SPI)",
                 uid_str,
                 iot_lock_state_str(new_lock),
                 iot_alarm_state_str(target_alarm),
                 alarm_pkt.seq_num);
        (void)lock_pkt;
    } else {
        mesh_rfid_auth_fail_t fail_pkt = {
            .seq_num = ++s_seq,
            .uid_len = uid_len,
            .local_attempt_hint = 1,
        };
        memcpy(fail_pkt.uid, uid, uid_len);
        ESP_LOGW(TAG, "RFID %s RECHAZADO [%s] -> Mesh 0xC001: OP_RFID_AUTH_FAIL seq=%u",
                 is_sim ? "(SIM)" : "(SPI)", uid_str, fail_pkt.seq_num);
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
        const TaskHandle_t handles[] = { s_h_rfid, s_h_hb, s_h_console };
        const char *const names[] = { "task_rfid_poll", "task_hb_n1", "task_console_n1" };
        str_metrics_log_health(&s_metrics, handles, names, 3);

        /* Alimenta TWDT en espera de 15 s dividida en 2 pasos < 10 s */
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
        ESP_LOGI(TAG, "Comando consola -> Cerradura ABIERTA");
    } else if (strcmp(line, "close") == 0) {
        lock_ctrl_set(LOCK_STATE_CERRADA);
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
        ESP_LOGI(TAG, "Estado N1: cerradura=%s rc522=%s",
                 iot_lock_state_str(lock_ctrl_get()),
                 rc522_spi_is_present() ? "OK" : "AUSENTE");
    } else if (strcmp(line, "stats") == 0) {
        const TaskHandle_t handles[] = { s_h_rfid, s_h_hb, s_h_console };
        const char *const names[] = { "task_rfid_poll", "task_hb_n1", "task_console_n1" };
        str_metrics_log_health(&s_metrics, handles, names, 3);
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
    status_led_set_state(STATUS_LED_UNPROVISIONED);
    ESP_ERROR_CHECK(boot_button_init(N1_BOOT_GPIO, NULL));

    ESP_ERROR_CHECK(rfid_store_init());
    ESP_ERROR_CHECK(lock_ctrl_init());
    rc522_spi_init(); /* No fatal si no hay módulo RC522 conectado en banco de pruebas */

    xTaskCreate(task_rfid_poll, "task_rfid_poll", 4096, NULL, 12, &s_h_rfid);
    xTaskCreate(task_hb_metrics_n1, "task_hb_n1", 3072, NULL, 3, &s_h_hb);
    xTaskCreate(task_console_n1, "task_console_n1", 4096, NULL, 2, &s_h_console);
}
