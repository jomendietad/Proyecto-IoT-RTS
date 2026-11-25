#pragma once

#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Estados visuales del LED RGB integrado según código unificado del proyecto. */
typedef enum {
    STATUS_LED_OFF = 0,
    STATUS_LED_UNPROVISIONED,   /**< Rojo fijo */
    STATUS_LED_PROVISIONING,    /**< Azul */
    STATUS_LED_ONLINE,          /**< Verde */
    STATUS_LED_WIFI_OFFLINE,    /**< Ámbar */
    STATUS_LED_ACTIVITY,        /**< Cian */
    STATUS_LED_ALARM_TRIGGERED, /**< Rojo intenso */
    STATUS_LED_FACTORY_RESET,   /**< Morado */
} status_led_state_t;

/**
 * @brief Inicializa el canal RMT TX para el LED WS2812 integrado.
 */
esp_err_t status_led_init(gpio_num_t gpio);

/**
 * @brief Fija el color correspondiente al estado del nodo.
 */
esp_err_t status_led_set_state(status_led_state_t state);

/**
 * @brief Emite un destello cian breve y restaura el estado previo.
 */
esp_err_t status_led_flash_activity(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
