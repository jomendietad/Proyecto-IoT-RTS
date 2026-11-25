#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "iot_common_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ble_central_rx_cb_t)(iot_node_role_t role,
                                    uint8_t opcode,
                                    const uint8_t *payload,
                                    size_t len,
                                    int64_t rx_ts_us,
                                    void *ctx);

typedef void (*ble_central_state_cb_t)(iot_node_role_t role,
                                       bool ready,
                                       void *ctx);

typedef struct {
    ble_central_rx_cb_t on_rx;
    ble_central_state_cb_t on_node_state;
    void *user_ctx;
} ble_central_cfg_t;

typedef struct {
    iot_node_role_t role;
    const char *name;
    bool ready;
    bool connecting;
    uint16_t conn_handle;
    int8_t rssi;
    uint32_t uptime_s;
} ble_central_node_info_t;

esp_err_t ble_central_star_init(const ble_central_cfg_t *cfg);

esp_err_t ble_central_star_send_cmd(iot_node_role_t role,
                                    uint8_t opcode,
                                    const void *payload,
                                    size_t len);

bool ble_central_star_get_info(iot_node_role_t role, ble_central_node_info_t *out_info);

uint8_t ble_central_star_connected_count(void);

#ifdef __cplusplus
}
#endif
