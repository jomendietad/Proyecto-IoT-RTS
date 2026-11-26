#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Snapshot del estado actual del supervisor Wi-Fi (failover dual). */
typedef struct {
    bool connected;
    bool configured;
    uint8_t active_slot; /**< 0 = primaria, 1 = respaldo */
    char ssid[33];
    char ip[16];
    int8_t rssi;
    uint32_t failovers;
} wifi_status_t;

typedef void (*wifi_state_cb_t)(bool connected, const wifi_status_t *status, void *ctx);

/**
 * @brief Inicializa Wi-Fi STA (coexistencia BLE + Wi-Fi) y lanza task_wifi_supervisor en Core 1.
 */
esp_err_t wifi_manager_init(wifi_state_cb_t cb, void *ctx, TaskHandle_t *out_task);

/**
 * @brief Retorna true si la interfaz STA tiene direccion IP activa.
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Copia el estado actual del enlace Wi-Fi y estadisticas de failover.
 */
void wifi_manager_get_status(wifi_status_t *out_status);

/**
 * @brief Fuerza conmutacion hacia la red alterna (si hay red de respaldo configurada).
 */
esp_err_t wifi_manager_force_failover(void);

#ifdef __cplusplus
}
#endif
