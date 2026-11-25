#include "temp_monitor.h"

#include <stdlib.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "temp_monitor";

#define CLIMA_NVS_NS       "clima_cfg"
#define CLIMA_NVS_KEY_HIGH "t_high_c"

static SemaphoreHandle_t s_mutex = NULL;
static uint8_t s_t_high_c = IOT_TEMP_THRESH_DEFAULT_C;
static bool s_has_prev = false;
static int16_t s_prev_temp_deci = 0;
static int16_t s_last_delta_deci = 0;
static bool s_high_active = false;
static iot_temp_alert_t s_alert_state = TEMP_ALERT_NORMAL;

static void save_threshold_nvs(uint8_t t_high_c)
{
    nvs_handle_t h;
    if (nvs_open(CLIMA_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, CLIMA_NVS_KEY_HIGH, t_high_c);
        nvs_commit(h);
        nvs_close(h);
    }
}

esp_err_t temp_monitor_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t h;
    if (nvs_open(CLIMA_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t saved = IOT_TEMP_THRESH_DEFAULT_C;
        if (nvs_get_u8(h, CLIMA_NVS_KEY_HIGH, &saved) == ESP_OK &&
            saved >= IOT_TEMP_THRESH_MIN_C && saved <= IOT_TEMP_THRESH_MAX_C) {
            s_t_high_c = saved;
        }
        nvs_close(h);
    }

    ESP_LOGI(TAG, "Monitor termico inicializado (T_high=%u C, hist=1.0 C, sobresalto=|dT|>=2.0 C)",
             s_t_high_c);
    return ESP_OK;
}

esp_err_t temp_monitor_set_threshold(uint8_t t_high_c)
{
    if (t_high_c < IOT_TEMP_THRESH_MIN_C || t_high_c > IOT_TEMP_THRESH_MAX_C) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_t_high_c = t_high_c;
    save_threshold_nvs(t_high_c);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void temp_monitor_evaluate(int16_t temp_deci_c, temp_eval_result_t *out)
{
    if (!s_mutex || !out) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int16_t delta_deci = 0;
    if (!s_has_prev) {
        s_has_prev = true;
        s_prev_temp_deci = temp_deci_c;
    } else {
        delta_deci = (int16_t)(temp_deci_c - s_prev_temp_deci);
        s_prev_temp_deci = temp_deci_c;
    }
    s_last_delta_deci = delta_deci;

    const bool is_spike = (abs((int)delta_deci) >= (int)IOT_TEMP_SPIKE_DELTA_DECI_C);

    const int16_t t_high_deci = (int16_t)s_t_high_c * 10;
    const bool prev_high = s_high_active;
    if (!s_high_active && temp_deci_c >= t_high_deci) {
        s_high_active = true;
    } else if (s_high_active && temp_deci_c < (t_high_deci - IOT_TEMP_HYSTERESIS_DECI_C)) {
        s_high_active = false;
    }
    const bool high_changed = (s_high_active != prev_high);

    iot_temp_alert_t st = TEMP_ALERT_NORMAL;
    if (s_high_active && is_spike) {
        st = TEMP_ALERT_BOTH;
    } else if (s_high_active) {
        st = TEMP_ALERT_HIGH;
    } else if (is_spike) {
        st = TEMP_ALERT_SPIKE;
    }
    s_alert_state = st;

    ble_clima_reason_t reason = BLE_CLIMA_REASON_PERIODIC;
    if (is_spike) {
        reason = BLE_CLIMA_REASON_TEMP_SPIKE;
    } else if (high_changed && s_high_active) {
        reason = BLE_CLIMA_REASON_ALERT_HIGH;
    } else if (high_changed && !s_high_active) {
        reason = BLE_CLIMA_REASON_NORMALIZED;
    }

    out->temp_deci_c = temp_deci_c;
    out->delta_deci_c = delta_deci;
    out->alert_state = st;
    out->reason = reason;
    out->threshold_high_c = s_t_high_c;
    out->should_notify = (is_spike || high_changed);

    xSemaphoreGive(s_mutex);
}

void temp_monitor_get_state(iot_temp_alert_t *out_alert,
                            uint8_t *out_t_high_c,
                            int16_t *out_last_temp_deci,
                            int16_t *out_last_delta_deci)
{
    if (!s_mutex) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (out_alert) {
        *out_alert = s_alert_state;
    }
    if (out_t_high_c) {
        *out_t_high_c = s_t_high_c;
    }
    if (out_last_temp_deci) {
        *out_last_temp_deci = s_has_prev ? s_prev_temp_deci : 0;
    }
    if (out_last_delta_deci) {
        *out_last_delta_deci = s_last_delta_deci;
    }
    xSemaphoreGive(s_mutex);
}
