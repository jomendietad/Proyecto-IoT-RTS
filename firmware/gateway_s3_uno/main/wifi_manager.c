#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#warning "secrets.h no encontrado; usando plantilla secrets.example.h (Wi-Fi/Telegram inactivos hasta configurar secrets.h)"
#include "secrets.example.h"
#endif

static const char *TAG = "wifi_mgr";

#define WIFI_RETRY_PER_SLOT      3U
#define WIFI_SUPERVISOR_TICK_MS  2000U

#define WIFI_EVT_DISCONNECTED_BIT    BIT0
#define WIFI_EVT_FORCE_FAILOVER_BIT  BIT1

static EventGroupHandle_t s_wifi_eg = NULL;
static SemaphoreHandle_t s_wifi_mutex = NULL;
static TaskHandle_t s_wifi_task = NULL;
static wifi_state_cb_t s_cb = NULL;
static void *s_cb_ctx = NULL;
static wifi_status_t s_status = { 0 };
static bool s_initialized = false;
static bool s_has_backup = false;
static volatile bool s_suppress_disc_evt = false;
static volatile bool s_connecting = false;

static bool is_placeholder_ssid(const char *ssid)
{
    if (!ssid || ssid[0] == '\0') {
        return true;
    }
    return (strcmp(ssid, "TU_WIFI_PRIMARIA") == 0);
}

static void slot_credentials(uint8_t slot, const char **out_ssid, const char **out_pass)
{
    if (slot == 1U && s_has_backup) {
        *out_ssid = WIFI_BACKUP_SSID;
        *out_pass = WIFI_BACKUP_PASS;
    } else {
        *out_ssid = WIFI_PRIMARY_SSID;
        *out_pass = WIFI_PRIMARY_PASS;
    }
}

static esp_err_t apply_slot_config(uint8_t slot)
{
    const char *ssid = NULL;
    const char *pass = NULL;
    slot_credentials(slot, &ssid, &pass);

    wifi_config_t wcfg = { 0 };
    strlcpy((char *)wcfg.sta.ssid, ssid, sizeof(wcfg.sta.ssid));
    strlcpy((char *)wcfg.sta.password, pass ? pass : "", sizeof(wcfg.sta.password));
    /* WIFI_AUTH_WPA_PSK (enum 2) acepta WPA, WPA2 puro (enum 3), WPA/WPA2 y WPA3 */
    wcfg.sta.threshold.authmode = (pass && pass[0] != '\0') ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wcfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wcfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wcfg.sta.pmf_cfg.capable = true;
    wcfg.sta.pmf_cfg.required = false;

    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    s_status.active_slot = slot;
    strlcpy(s_status.ssid, ssid, sizeof(s_status.ssid));
    s_status.ip[0] = '\0';
    s_status.rssi = 0;
    xSemaphoreGive(s_wifi_mutex);

    ESP_LOGI(TAG, "Configurando Wi-Fi STA slot=%u SSID='%s'", (unsigned int)slot, ssid);
    return esp_wifi_set_config(WIFI_IF_STA, &wcfg);
}

static void notify_state_cb(bool connected)
{
    if (!s_cb) {
        return;
    }
    wifi_status_t snap;
    wifi_manager_get_status(&snap);
    s_cb(connected, &snap, s_cb_ctx);
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi STA iniciado, conectando...");
        s_connecting = true;
        (void)esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)event_data;
        bool was_connected = false;
        s_connecting = false;

        xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
        was_connected = s_status.connected;
        s_status.connected = false;
        s_status.ip[0] = '\0';
        s_status.rssi = 0;
        xSemaphoreGive(s_wifi_mutex);

        ESP_LOGW(TAG, "Wi-Fi desconectado (reason=%u)", disc ? (unsigned int)disc->reason : 0U);
        if (was_connected) {
            notify_state_cb(false);
        }
        if (s_wifi_eg && !s_suppress_disc_evt) {
            xEventGroupSetBits(s_wifi_eg, WIFI_EVT_DISCONNECTED_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ip_evt = (const ip_event_got_ip_t *)event_data;
        wifi_ap_record_t ap = { 0 };
        const int8_t rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
        char ip_copy[16] = { 0 };
        char ssid_copy[33] = { 0 };
        uint8_t slot = 0;

        s_connecting = false;

        xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
        s_status.connected = true;
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&ip_evt->ip_info.ip));
        s_status.rssi = rssi;
        strlcpy(ip_copy, s_status.ip, sizeof(ip_copy));
        strlcpy(ssid_copy, s_status.ssid, sizeof(ssid_copy));
        slot = s_status.active_slot;
        xSemaphoreGive(s_wifi_mutex);

        ESP_LOGI(TAG, "Wi-Fi ONLINE slot=%u SSID='%s' IP=%s RSSI=%d dBm",
                 (unsigned int)slot, ssid_copy, ip_copy, rssi);

        if (s_wifi_eg) {
            xEventGroupClearBits(s_wifi_eg, WIFI_EVT_DISCONNECTED_BIT);
        }
        notify_state_cb(true);
    }
}

static void task_wifi_supervisor(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);

    uint8_t retry_cnt = 0;
    uint8_t rssi_poll_ticks = 0;

    while (1) {
        xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
        const bool configured = s_status.configured;
        const bool connected = s_status.connected;
        const uint8_t cur_slot = s_status.active_slot;
        xSemaphoreGive(s_wifi_mutex);

        if (!configured) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(WIFI_SUPERVISOR_TICK_MS));
            continue;
        }

        const EventBits_t bits = xEventGroupWaitBits(
            s_wifi_eg,
            WIFI_EVT_DISCONNECTED_BIT | WIFI_EVT_FORCE_FAILOVER_BIT,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(WIFI_SUPERVISOR_TICK_MS));

        esp_task_wdt_reset();

        if (bits & WIFI_EVT_FORCE_FAILOVER_BIT) {
            if (s_has_backup) {
                const uint8_t next_slot = (cur_slot == 0U) ? 1U : 0U;
                xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
                s_status.failovers++;
                xSemaphoreGive(s_wifi_mutex);

                ESP_LOGW(TAG, "Failover Wi-Fi forzado: slot %u -> slot %u",
                         (unsigned int)cur_slot, (unsigned int)next_slot);
                s_suppress_disc_evt = true;
                (void)esp_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(400));
                esp_task_wdt_reset();
                xEventGroupClearBits(s_wifi_eg, WIFI_EVT_DISCONNECTED_BIT);
                s_suppress_disc_evt = false;

                retry_cnt = 0;
                rssi_poll_ticks = 0;
                (void)apply_slot_config(next_slot);
                s_connecting = true;
                (void)esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "Failover solicitado sin red de respaldo configurada");
            }
            continue;
        }

        if (bits & WIFI_EVT_DISCONNECTED_BIT) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_task_wdt_reset();

            xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
            const bool now_conn = s_status.connected;
            const uint8_t slot_now = s_status.active_slot;
            xSemaphoreGive(s_wifi_mutex);

            if (now_conn || s_connecting) {
                continue;
            }

            retry_cnt++;
            if (retry_cnt < WIFI_RETRY_PER_SLOT) {
                ESP_LOGI(TAG, "Reintento Wi-Fi (%u/%u) en slot=%u...",
                         (unsigned int)retry_cnt, (unsigned int)WIFI_RETRY_PER_SLOT,
                         (unsigned int)slot_now);
                s_connecting = true;
                (void)esp_wifi_connect();
            } else {
                retry_cnt = 0;
                if (s_has_backup) {
                    const uint8_t next_slot = (slot_now == 0U) ? 1U : 0U;
                    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
                    s_status.failovers++;
                    xSemaphoreGive(s_wifi_mutex);

                    ESP_LOGW(TAG, "Failover Wi-Fi automatico tras %u fallos: slot %u -> slot %u",
                             (unsigned int)WIFI_RETRY_PER_SLOT,
                             (unsigned int)slot_now,
                             (unsigned int)next_slot);
                    (void)apply_slot_config(next_slot);
                } else {
                    ESP_LOGW(TAG, "Sin red secundaria; reintentando red primaria...");
                }
                s_connecting = true;
                (void)esp_wifi_connect();
            }
            continue;
        }

        /* Timeout periodico de 2000 ms */
        if (connected) {
            retry_cnt = 0;
            rssi_poll_ticks++;
            if (rssi_poll_ticks >= 5U) {
                rssi_poll_ticks = 0;
                wifi_ap_record_t ap = { 0 };
                if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
                    s_status.rssi = ap.rssi;
                    xSemaphoreGive(s_wifi_mutex);
                }
            }
        } else if (!s_connecting) {
            ESP_LOGI(TAG, "Recuperacion Wi-Fi periodica en slot=%u...", (unsigned int)cur_slot);
            s_connecting = true;
            (void)esp_wifi_connect();
        }
    }
}

esp_err_t wifi_manager_init(wifi_state_cb_t cb, void *ctx, TaskHandle_t *out_task)
{
    if (s_initialized) {
        if (out_task) {
            *out_task = s_wifi_task;
        }
        return ESP_OK;
    }

    s_cb = cb;
    s_cb_ctx = ctx;
    s_wifi_eg = xEventGroupCreate();
    s_wifi_mutex = xSemaphoreCreateMutex();
    if (!s_wifi_eg || !s_wifi_mutex) {
        return ESP_ERR_NO_MEM;
    }

    s_has_backup = (WIFI_BACKUP_SSID[0] != '\0');
    const bool primary_ok = !is_placeholder_ssid(WIFI_PRIMARY_SSID);

    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    memset(&s_status, 0, sizeof(s_status));
    s_status.configured = primary_ok;
    if (primary_ok) {
        strlcpy(s_status.ssid, WIFI_PRIMARY_SSID, sizeof(s_status.ssid));
    }
    xSemaphoreGive(s_wifi_mutex);

    if (!primary_ok) {
        ESP_LOGW(TAG, "Credenciales Wi-Fi no configuradas en secrets.h; operando en modo estrella BLE offline");
    } else {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        (void)esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(apply_slot_config(0U));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
        ESP_LOGI(TAG, "Wi-Fi STA listo (coexistencia WIFI_PS_MIN_MODEM, backup=%s)",
                 s_has_backup ? "SI" : "NO");
    }

    const BaseType_t ok = xTaskCreatePinnedToCore(task_wifi_supervisor,
                                                  "task_wifi_sup",
                                                  4096,
                                                  NULL,
                                                  4,
                                                  &s_wifi_task,
                                                  1);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (out_task) {
        *out_task = s_wifi_task;
    }
    s_initialized = true;
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    if (!s_wifi_mutex) {
        return false;
    }
    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    const bool conn = s_status.connected;
    xSemaphoreGive(s_wifi_mutex);
    return conn;
}

void wifi_manager_get_status(wifi_status_t *out_status)
{
    if (!out_status) {
        return;
    }
    if (!s_wifi_mutex) {
        memset(out_status, 0, sizeof(*out_status));
        return;
    }
    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    *out_status = s_status;
    xSemaphoreGive(s_wifi_mutex);
}

esp_err_t wifi_manager_force_failover(void)
{
    if (!s_initialized || !s_wifi_eg || !s_status.configured) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_has_backup) {
        return ESP_ERR_NOT_FOUND;
    }
    xEventGroupSetBits(s_wifi_eg, WIFI_EVT_FORCE_FAILOVER_BIT);
    return ESP_OK;
}
