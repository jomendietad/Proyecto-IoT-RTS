#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*boot_reset_cb_t)(void);

/**
 * @brief Inicia tarea de supervisión del botón BOOT (mantener >= 5 s para factory reset NVS).
 */
esp_err_t boot_button_init(gpio_num_t gpio, boot_reset_cb_t on_factory_reset);

#ifdef __cplusplus
}
#endif
