#include "dht11_rmt.h"

#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dht11_rmt";

/* Pinout exacto según docs/hardware/pinout.md (Nodo 3 ESP32-C6 SuperMini) */
#define DHT11_GPIO        GPIO_NUM_2
#define DHT11_RMT_RES_HZ  1000000U /* 1 us por tick */

static rmt_channel_handle_t s_rx_chan = NULL;
static TaskHandle_t s_wait_task = NULL;

static bool IRAM_ATTR dht11_rx_done_cb(rmt_channel_handle_t channel,
                                       const rmt_rx_done_event_data_t *edata,
                                       void *user_ctx)
{
    (void)channel;
    (void)user_ctx;
    BaseType_t hpw = pdFALSE;
    if (s_wait_task) {
        xTaskNotifyFromISR(s_wait_task, (uint32_t)edata->num_symbols, eSetValueWithOverwrite, &hpw);
    }
    return hpw == pdTRUE;
}

esp_err_t dht11_rmt_init(void)
{
    if (s_rx_chan != NULL) {
        return ESP_OK;
    }

    const rmt_rx_channel_config_t rx_cfg = {
        .gpio_num = DHT11_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = DHT11_RMT_RES_HZ,
        .mem_block_symbols = 48,
        .intr_priority = 0,
    };
    esp_err_t err = rmt_new_rx_channel(&rx_cfg, &s_rx_chan);
    if (err != ESP_OK) {
        return err;
    }

    const rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = dht11_rx_done_cb,
    };
    err = rmt_rx_register_event_callbacks(s_rx_chan, &cbs, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = rmt_enable(s_rx_chan);
    if (err != ESP_OK) {
        return err;
    }

    /* Drenador abierto + pull-up interno para bus 1-wire bidireccional */
    ESP_ERROR_CHECK(gpio_set_direction(DHT11_GPIO, GPIO_MODE_INPUT_OUTPUT_OD));
    ESP_ERROR_CHECK(gpio_set_pull_mode(DHT11_GPIO, GPIO_PULLUP_ONLY));
    ESP_ERROR_CHECK(gpio_set_level(DHT11_GPIO, 1));

    ESP_LOGI(TAG, "DHT11 RMT RX inicializado en GPIO %d (1 MHz)", DHT11_GPIO);
    return ESP_OK;
}

esp_err_t dht11_rmt_read(int16_t *out_temp_deci_c, uint8_t *out_hum_pct)
{
    if (!s_rx_chan || !out_temp_deci_c || !out_hum_pct) {
        return ESP_ERR_INVALID_STATE;
    }

    rmt_symbol_word_t symbols[64];
    uint16_t high_pulses[64];
    size_t high_count = 0;

    s_wait_task = xTaskGetCurrentTaskHandle();
    (void)xTaskNotifyStateClear(NULL);

    /* Pulso de inicio: 20 ms en bajo sin bloquear planificador FreeRTOS */
    gpio_set_level(DHT11_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    const rmt_receive_config_t recv_cfg = {
        .signal_range_min_ns = 1000U,
        .signal_range_max_ns = 200000U,
    };
    esp_err_t err = rmt_receive(s_rx_chan, symbols, sizeof(symbols), &recv_cfg);
    gpio_set_level(DHT11_GPIO, 1);

    if (err != ESP_OK) {
        s_wait_task = NULL;
        return err;
    }

    uint32_t num_sym = 0;
    if (xTaskNotifyWait(0, 0xFFFFFFFFUL, &num_sym, pdMS_TO_TICKS(50)) != pdTRUE) {
        s_wait_task = NULL;
        return ESP_ERR_TIMEOUT;
    }
    s_wait_task = NULL;

    if (num_sym > 64U) {
        num_sym = 64U;
    }

    for (size_t i = 0; i < num_sym; i++) {
        uint16_t h = 0;
        if (symbols[i].level0 == 1 && symbols[i].duration0 > 0) {
            h = symbols[i].duration0;
        } else if (symbols[i].level1 == 1 && symbols[i].duration1 > 0) {
            h = symbols[i].duration1;
        }
        if (h > 0U && high_count < 64U) {
            high_pulses[high_count++] = h;
        }
    }

    if (high_count < 40U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const size_t start = high_count - 40U;
    uint8_t data[5] = { 0 };
    for (size_t bit = 0; bit < 40U; bit++) {
        data[bit / 8U] <<= 1U;
        if (high_pulses[start + bit] > 45U) {
            data[bit / 8U] |= 1U;
        }
    }

    const uint8_t csum = (uint8_t)((data[0] + data[1] + data[2] + data[3]) & 0xFFU);
    if (csum != data[4]) {
        return ESP_ERR_INVALID_CRC;
    }

    *out_hum_pct = data[0];
    int16_t temp = (int16_t)(data[2] * 10 + (data[3] & 0x0FU));
    if (data[3] & 0x80U) {
        temp = (int16_t)(-temp);
    }
    *out_temp_deci_c = temp;
    return ESP_OK;
}
