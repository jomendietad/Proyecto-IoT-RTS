#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_common_types.h"
#include "ble_protocol.h"
#include "str_metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t star_coordinator_init(str_metrics_ctx_t *metrics, TaskHandle_t *out_fsm_task);

void star_coordinator_on_ble_rx(iot_node_role_t role,
                                uint8_t opcode,
                                const uint8_t *payload,
                                size_t len,
                                int64_t rx_ts_us,
                                void *ctx);

void star_coordinator_on_node_state(iot_node_role_t role, bool ready, void *ctx);

esp_err_t star_cmd_alarm(iot_alarm_state_t target);

esp_err_t star_cmd_lock(iot_lock_state_t target);

esp_err_t star_cmd_rfid(ble_rfid_action_t action, const uint8_t *uid, uint8_t uid_len);

esp_err_t star_cmd_threshold(uint8_t new_t_high_c);

void star_cmd_request_status_all(uint8_t req_flags);

void star_log_global_status(void);

void star_log_ble_network(void);

#ifdef __cplusplus
}
#endif
