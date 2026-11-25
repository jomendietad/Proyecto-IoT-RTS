#include "fan_pwm.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "fan_pwm";

/* Pinout y parámetros exactos según docs/hardware/pinout.md y fsm_y_telegram.md */
#define FAN_PWM_GPIO       GPIO_NUM_3
#define FAN_LEDC_TIMER     LEDC_TIMER_0
#define FAN_LEDC_CHANNEL   LEDC_CHANNEL_0
#define FAN_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define FAN_PWM_FREQ_HZ    5000U

#define FAN_DUTY_OFF       0U
#define FAN_DUTY_BAJA      358U  /* ~35 % de 1023 */
#define FAN_DUTY_MEDIA     716U  /* ~70 % de 1023 */
#define FAN_DUTY_ALTA      1023U /* 100 % */

#define CLIMA_NVS_NS       "clima_cfg"
#define CLIMA_NVS_KEY_T    "t_base_c"

static SemaphoreHandle_t s_mutex = NULL;
static iot_fan_mode_t s_mode = FAN_MODE_AUTO;
static iot_fan_level_t s_level = FAN_LEVEL_OFF;
static uint8_t s_t_base_c = IOT_FAN_TBASE_DEFAULT_C;
static int16_t s_last_temp_deci_c = 220;

static uint32_t level_to_duty(iot_fan_level_t lvl)
{
    switch (lvl) {
    case FAN_LEVEL_BAJA:  return FAN_DUTY_BAJA;
    case FAN_LEVEL_MEDIA: return FAN_DUTY_MEDIA;
    case FAN_LEVEL_ALTA:  return FAN_DUTY_ALTA;
    case FAN_LEVEL_OFF:
    default:              return FAN_DUTY_OFF;
    }
}

static esp_err_t apply_duty_locked(iot_fan_level_t lvl)
{
    const uint32_t duty = level_to_duty(lvl);
    esp_err_t err = ledc_set_duty(FAN_LEDC_MODE, FAN_LEDC_CHANNEL, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(FAN_LEDC_MODE, FAN_LEDC_CHANNEL);
    }
    return err;
}

static iot_fan_level_t compute_auto_level(int16_t temp_deci, iot_fan_level_t cur, uint8_t t_base_c)
{
    const int16_t tb = (int16_t)t_base_c * 10;
    const int16_t hyst = IOT_FAN_HYSTERESIS_DECI_C; /* 10 = 1.0 °C */

    /* Subidas máximas primero para soportar saltos bruscos */
    if (temp_deci >= (tb + 60)) {
        return FAN_LEVEL_ALTA;
    }
    if (cur == FAN_LEVEL_ALTA) {
        if (temp_deci >= (tb + 60 - hyst)) {
            return FAN_LEVEL_ALTA;
        }
    }

    if (temp_deci >= (tb + 30)) {
        return FAN_LEVEL_MEDIA;
    }
    if (cur == FAN_LEVEL_MEDIA || cur == FAN_LEVEL_ALTA) {
        if (temp_deci >= (tb + 30 - hyst)) {
            return FAN_LEVEL_MEDIA;
        }
    }

    if (temp_deci >= tb) {
        return FAN_LEVEL_BAJA;
    }
    if (cur != FAN_LEVEL_OFF) {
        if (temp_deci >= (tb - hyst)) {
            return FAN_LEVEL_BAJA;
        }
    }

    return FAN_LEVEL_OFF;
}

static void save_threshold_nvs(uint8_t t_base_c)
{
    nvs_handle_t h;
    if (nvs_open(CLIMA_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, CLIMA_NVS_KEY_T, t_base_c);
        nvs_commit(h);
        nvs_close(h);
    }
}

esp_err_t fan_pwm_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t h;
    if (nvs_open(CLIMA_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t saved = IOT_FAN_TBASE_DEFAULT_C;
        if (nvs_get_u8(h, CLIMA_NVS_KEY_T, &saved) == ESP_OK &&
            saved >= IOT_FAN_TBASE_MIN_C && saved <= IOT_FAN_TBASE_MAX_C) {
            s_t_base_c = saved;
        }
        nvs_close(h);
    }

    const ledc_timer_config_t t_cfg = {
        .speed_mode = FAN_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = FAN_LEDC_TIMER,
        .freq_hz = FAN_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&t_cfg);
    if (err != ESP_OK) {
        return err;
    }

    const ledc_channel_config_t c_cfg = {
        .gpio_num = FAN_PWM_GPIO,
        .speed_mode = FAN_LEDC_MODE,
        .channel = FAN_LEDC_CHANNEL,
        .timer_sel = FAN_LEDC_TIMER,
        .duty = FAN_DUTY_OFF,
        .hpoint = 0,
    };
    err = ledc_channel_config(&c_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Ventilador PWM LEDC inicializado en GPIO %d (5 kHz, 10 bits, T_base=%u C)",
                 FAN_PWM_GPIO, s_t_base_c);
    }
    return err;
}

esp_err_t fan_pwm_set_manual(iot_fan_level_t level)
{
    if (!s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_mode = FAN_MODE_MANUAL;
    s_level = level;
    esp_err_t err = apply_duty_locked(level);
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t fan_pwm_set_auto(void)
{
    if (!s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_mode = FAN_MODE_AUTO;
    s_level = compute_auto_level(s_last_temp_deci_c, s_level, s_t_base_c);
    esp_err_t err = apply_duty_locked(s_level);
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t fan_pwm_set_threshold(uint8_t t_base_c)
{
    if (t_base_c < IOT_FAN_TBASE_MIN_C || t_base_c > IOT_FAN_TBASE_MAX_C) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_t_base_c = t_base_c;
    save_threshold_nvs(t_base_c);
    if (s_mode == FAN_MODE_AUTO) {
        s_level = compute_auto_level(s_last_temp_deci_c, s_level, s_t_base_c);
        apply_duty_locked(s_level);
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

bool fan_pwm_update_from_temp(int16_t temp_deci_c)
{
    if (!s_mutex) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_last_temp_deci_c = temp_deci_c;
    if (s_mode != FAN_MODE_AUTO) {
        xSemaphoreGive(s_mutex);
        return false;
    }

    const iot_fan_level_t next = compute_auto_level(temp_deci_c, s_level, s_t_base_c);
    const bool changed = (next != s_level);
    if (changed) {
        s_level = next;
        apply_duty_locked(next);
    }
    xSemaphoreGive(s_mutex);
    return changed;
}

void fan_pwm_get_state(iot_fan_mode_t *out_mode,
                       iot_fan_level_t *out_level,
                       uint8_t *out_t_base_c)
{
    if (!s_mutex) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (out_mode) {
        *out_mode = s_mode;
    }
    if (out_level) {
        *out_level = s_level;
    }
    if (out_t_base_c) {
        *out_t_base_c = s_t_base_c;
    }
    xSemaphoreGive(s_mutex);
}
