#include "lock_ctrl.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "lock_ctrl";

#define LOCK_LED_GPIO GPIO_NUM_20

static SemaphoreHandle_t s_lock_mutex = NULL;
static iot_lock_state_t s_state = LOCK_STATE_CERRADA;

esp_err_t lock_ctrl_init(void)
{
    s_lock_mutex = xSemaphoreCreateMutex();
    if (!s_lock_mutex) {
        return ESP_ERR_NO_MEM;
    }

    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LOCK_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    s_state = LOCK_STATE_CERRADA;
    gpio_set_level(LOCK_LED_GPIO, 0);
    ESP_LOGI(TAG, "Cerradura inicializada en GPIO %d (estado=%s)", LOCK_LED_GPIO, iot_lock_state_str(s_state));
    return ESP_OK;
}

esp_err_t lock_ctrl_set(iot_lock_state_t state)
{
    if (!s_lock_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock_mutex, portMAX_DELAY);
    s_state = state;
    gpio_set_level(LOCK_LED_GPIO, (state == LOCK_STATE_ABIERTA) ? 1 : 0);
    xSemaphoreGive(s_lock_mutex);
    return ESP_OK;
}

iot_lock_state_t lock_ctrl_toggle(void)
{
    if (!s_lock_mutex) {
        return LOCK_STATE_CERRADA;
    }
    xSemaphoreTake(s_lock_mutex, portMAX_DELAY);
    s_state = (s_state == LOCK_STATE_CERRADA) ? LOCK_STATE_ABIERTA : LOCK_STATE_CERRADA;
    gpio_set_level(LOCK_LED_GPIO, (s_state == LOCK_STATE_ABIERTA) ? 1 : 0);
    const iot_lock_state_t res = s_state;
    xSemaphoreGive(s_lock_mutex);
    return res;
}

iot_lock_state_t lock_ctrl_get(void)
{
    if (!s_lock_mutex) {
        return LOCK_STATE_CERRADA;
    }
    xSemaphoreTake(s_lock_mutex, portMAX_DELAY);
    const iot_lock_state_t res = s_state;
    xSemaphoreGive(s_lock_mutex);
    return res;
}
