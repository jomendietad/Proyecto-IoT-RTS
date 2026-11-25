#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa el canal RMT RX para lectura no bloqueante del DHT11 en GPIO 2.
 */
esp_err_t dht11_rmt_init(void);

/**
 * @brief Lee temperatura (décimas de °C) y humedad (%) usando periférico RMT.
 */
esp_err_t dht11_rmt_read(int16_t *out_temp_deci_c, uint8_t *out_hum_pct);

#ifdef __cplusplus
}
#endif
