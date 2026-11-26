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

/** @brief Snapshot del enlace y ultimos reportes recibidos de un nodo C6. */
typedef struct {
    bool connected;
    bool offline_warned;
    ble_heartbeat_t hb;
    ble_metrics_report_t metrics;
    bool has_hb;
    bool has_metrics;
} star_node_snapshot_t;

/** @brief Snapshot atomico del estado global consolidado en el Maestro S3. */
typedef struct {
    star_node_snapshot_t nodes[3];
    iot_lock_state_t lock_state;
    iot_alarm_state_t alarm_state;
    uint8_t alarm_fail_cnt;
    bool pir_active;
    int16_t temp_deci_c;
    int16_t delta_deci_c;
    uint8_t humidity_pct;
    iot_temp_alert_t temp_alert;
    uint8_t t_high_c;
    bool clima_valid;
    bool wifi_online;
} star_global_snapshot_t;

esp_err_t star_coordinator_init(str_metrics_ctx_t *metrics, TaskHandle_t *out_fsm_task);

void star_coordinator_on_ble_rx(iot_node_role_t role,
                                uint8_t opcode,
                                const uint8_t *payload,
                                size_t len,
                                int64_t rx_ts_us,
                                void *ctx);

void star_coordinator_on_node_state(iot_node_role_t role, bool ready, void *ctx);

void star_coordinator_set_wifi_online(bool online);

void star_coordinator_get_snapshot(star_global_snapshot_t *out);

bool star_parse_uid_hex(const char *hex, uint8_t *out_uid, uint8_t *out_len);

void star_format_uid_hex(const uint8_t *uid, uint8_t len, char *out, size_t out_sz);

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
