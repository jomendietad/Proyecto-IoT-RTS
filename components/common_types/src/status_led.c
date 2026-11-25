#include "status_led.h"

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "status_led";

#define WS2812_RES_HZ 10000000U /* 100 ns por tick */

static rmt_channel_handle_t s_tx_chan = NULL;
static rmt_encoder_handle_t s_bytes_enc = NULL;
static SemaphoreHandle_t s_led_mutex = NULL;
static status_led_state_t s_current_state = STATUS_LED_OFF;
static uint8_t s_grb_buf[3] = {0, 0, 0};

static void state_to_grb(status_led_state_t state, uint8_t grb[3])
{
    /* Orden WS2812: G, R, B (brillo moderado para laboratorio) */
    switch (state) {
    case STATUS_LED_UNPROVISIONED:   grb[0] = 0;  grb[1] = 24; grb[2] = 0;  break;
    case STATUS_LED_PROVISIONING:    grb[0] = 0;  grb[1] = 0;  grb[2] = 24; break;
    case STATUS_LED_ONLINE:          grb[0] = 24; grb[1] = 0;  grb[2] = 0;  break;
    case STATUS_LED_WIFI_OFFLINE:    grb[0] = 18; grb[1] = 24; grb[2] = 0;  break;
    case STATUS_LED_ACTIVITY:        grb[0] = 20; grb[1] = 0;  grb[2] = 24; break;
    case STATUS_LED_ALARM_TRIGGERED: grb[0] = 0;  grb[1] = 48; grb[2] = 0;  break;
    case STATUS_LED_FACTORY_RESET:   grb[0] = 0;  grb[1] = 24; grb[2] = 24; break;
    case STATUS_LED_OFF:
    default:                         grb[0] = 0;  grb[1] = 0;  grb[2] = 0;  break;
    }
}

static esp_err_t write_grb_locked(const uint8_t grb[3])
{
    if (!s_tx_chan || !s_bytes_enc) {
        return ESP_ERR_INVALID_STATE;
    }

    s_grb_buf[0] = grb[0];
    s_grb_buf[1] = grb[1];
    s_grb_buf[2] = grb[2];

    const rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
        .flags.eot_level = 0,
    };

    esp_err_t err = rmt_transmit(s_tx_chan, s_bytes_enc, s_grb_buf, sizeof(s_grb_buf), &tx_cfg);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_tx_chan, pdMS_TO_TICKS(20));
    }
    return err;
}

esp_err_t status_led_init(gpio_num_t gpio)
{
    if (s_tx_chan != NULL) {
        return ESP_OK;
    }

    s_led_mutex = xSemaphoreCreateMutex();
    if (!s_led_mutex) {
        return ESP_ERR_NO_MEM;
    }

    const rmt_tx_channel_config_t chan_cfg = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = WS2812_RES_HZ,
        .mem_block_symbols = 48,
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&chan_cfg, &s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo crear canal RMT TX en GPIO %d (%s)", gpio, esp_err_to_name(err));
        return err;
    }

    const rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = {
            .level0 = 1,
            .duration0 = 3, /* 0.3 us */
            .level1 = 0,
            .duration1 = 9, /* 0.9 us */
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = 9, /* 0.9 us */
            .level1 = 0,
            .duration1 = 3, /* 0.3 us */
        },
        .flags.msb_first = 1,
    };
    err = rmt_new_bytes_encoder(&enc_cfg, &s_bytes_enc);
    if (err != ESP_OK) {
        return err;
    }

    err = rmt_enable(s_tx_chan);
    if (err != ESP_OK) {
        return err;
    }

    return status_led_set_state(STATUS_LED_OFF);
}

esp_err_t status_led_set_state(status_led_state_t state)
{
    if (!s_led_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t grb[3];
    state_to_grb(state, grb);

    if (xSemaphoreTake(s_led_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_current_state = state;
    esp_err_t err = write_grb_locked(grb);
    xSemaphoreGive(s_led_mutex);
    return err;
}

esp_err_t status_led_flash_activity(uint32_t duration_ms)
{
    if (!s_led_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t flash_grb[3];
    uint8_t restore_grb[3];
    state_to_grb(STATUS_LED_ACTIVITY, flash_grb);

    if (xSemaphoreTake(s_led_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    state_to_grb(s_current_state, restore_grb);
    esp_err_t err = write_grb_locked(flash_grb);
    xSemaphoreGive(s_led_mutex);

    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    if (xSemaphoreTake(s_led_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        state_to_grb(s_current_state, restore_grb);
        err = write_grb_locked(restore_grb);
        xSemaphoreGive(s_led_mutex);
    }
    return err;
}
