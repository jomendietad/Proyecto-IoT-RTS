#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configura GPIO 3 para PIR HC-SR501 con interrupción por flanco de subida.
 */
esp_err_t pir_sensor_init(TaskHandle_t alarm_task, uint32_t notify_bit, volatile int64_t *isr_ts_out);

/**
 * @brief Retorna true si la señal digital del PIR está en nivel alto actualmente.
 */
bool pir_sensor_is_active(void);

#ifdef __cplusplus
}
#endif
