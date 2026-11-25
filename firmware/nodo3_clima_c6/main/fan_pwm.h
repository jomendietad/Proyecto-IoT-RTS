#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "iot_common_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t fan_pwm_init(void);

esp_err_t fan_pwm_set_manual(iot_fan_level_t level);

esp_err_t fan_pwm_set_auto(void);

esp_err_t fan_pwm_set_threshold(uint8_t t_base_c);

/**
 * @brief Evalúa temperatura actual si está en modo AUTO y actualiza PWM.
 * @return true si cambió el nivel de velocidad del ventilador.
 */
bool fan_pwm_update_from_temp(int16_t temp_deci_c);

void fan_pwm_get_state(iot_fan_mode_t *out_mode,
                       iot_fan_level_t *out_level,
                       uint8_t *out_t_base_c);

#ifdef __cplusplus
}
#endif
