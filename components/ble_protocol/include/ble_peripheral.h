#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback invocado en contexto de la tarea NimBLE al recibir escritura en CMD_RX (0xFF12).
 *        No debe bloquear ni realizar llamadas lentas de I/O; despachar a una cola FreeRTOS.
 */
typedef void (*ble_peripheral_cmd_cb_t)(uint8_t opcode,
                                        const uint8_t *payload,
                                        size_t len,
                                        void *ctx);

/**
 * @brief Callback de cambio de estado de conexión / suscripción Notify (CCCD).
 */
typedef void (*ble_peripheral_conn_cb_t)(bool connected, bool subscribed, void *ctx);

typedef struct {
    const char *device_name;
    ble_peripheral_cmd_cb_t on_cmd;
    ble_peripheral_conn_cb_t on_conn;
    void *user_ctx;
} ble_peripheral_cfg_t;

/**
 * @brief Inicializa NimBLE en modo Peripheral + GATT Server (servicio 0xFF10) e inicia Advertising.
 */
esp_err_t ble_peripheral_init(const ble_peripheral_cfg_t *cfg);

/**
 * @brief Envía notificación GATT por 0xFF11 con formato [opcode | payload].
 */
esp_err_t ble_peripheral_notify(uint8_t opcode, const void *payload, size_t len);

/**
 * @brief Retorna true si existe una conexión BLE activa con el Maestro S3.
 */
bool ble_peripheral_is_connected(void);

/**
 * @brief Retorna true si el Maestro S3 está conectado y suscrito a Notify (0xFF11).
 */
bool ble_peripheral_is_subscribed(void);

#ifdef __cplusplus
}
#endif
