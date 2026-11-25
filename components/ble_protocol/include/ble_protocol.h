#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "iot_common_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Identificación BLE GATT en estrella (NimBLE)
 * ============================================================================ */
#define BLE_ADV_NAME_NODO1        "IOT-N1-ACCESO"
#define BLE_ADV_NAME_NODO2        "IOT-N2-SEGURIDAD"
#define BLE_ADV_NAME_NODO3        "IOT-N3-CLIMA"

#define BLE_GATT_SVC_UUID         0xFF10U
#define BLE_GATT_CHR_EVT_TX_UUID  0xFF11U /**< Notify: C6 -> Maestro S3 */
#define BLE_GATT_CHR_CMD_RX_UUID  0xFF12U /**< Write with Response: Maestro S3 -> C6 */

#define BLE_PROTO_VERSION         0x01U

#define BLE_NODE_ID_GATEWAY_S3      0x0001U
#define BLE_NODE_ID_NODO1_ACCESO    0x0010U
#define BLE_NODE_ID_NODO2_SEGURIDAD 0x0020U
#define BLE_NODE_ID_NODO3_CLIMA     0x0030U

/* ============================================================================
 * Opcodes del protocolo binario compacto sobre BLE GATT (estrella)
 * ============================================================================ */
#define OP_ALARM_SET              0xC0U
#define OP_ALARM_EVENT            0xC1U
#define OP_RFID_AUTH_FAIL         0xC2U
#define OP_LOCK_SET               0xC3U
#define OP_LOCK_STATUS            0xC4U
#define OP_RFID_MGMT              0xC5U
#define OP_RFID_MGMT_ACK          0xC6U
#define OP_CLIMA_TELEMETRY        0xC7U
#define OP_THRESHOLD_SET          0xC9U
#define OP_HEARTBEAT              0xCAU
#define OP_STATUS_REQ             0xCBU
#define OP_METRICS_REPORT         0xCCU
#define OP_PROTO_ACK              0xCEU

typedef enum {
    BLE_SRC_TELEGRAM   = 1,
    BLE_SRC_RFID_NODO1 = 2,
} ble_alarm_cmd_source_t;

typedef enum {
    BLE_LOCK_SRC_TELEGRAM   = 0,
    BLE_LOCK_SRC_RFID_VALID = 1,
} ble_lock_src_t;

typedef enum {
    BLE_RFID_ACT_LIST = 0,
    BLE_RFID_ACT_ADD  = 1,
    BLE_RFID_ACT_DEL  = 2,
} ble_rfid_action_t;

typedef enum {
    BLE_CLIMA_REASON_PERIODIC   = 0,
    BLE_CLIMA_REASON_ALERT_HIGH = 1,
    BLE_CLIMA_REASON_TEMP_SPIKE = 2,
    BLE_CLIMA_REASON_NORMALIZED = 3,
    BLE_CLIMA_REASON_SENSOR_ERR = 4,
} ble_clima_reason_t;

/* ============================================================================
 * Payloads binarios empaquetados (little-endian)
 * ============================================================================ */

typedef struct __attribute__((packed)) {
    uint8_t seq_num;
    uint8_t target_state; /**< iot_alarm_state_t (0=DESARMADA, 1=ARMADA) */
    uint8_t source;       /**< ble_alarm_cmd_source_t */
} ble_alarm_set_t;
_Static_assert(sizeof(ble_alarm_set_t) == 3, "ble_alarm_set_t debe medir 3 bytes");

typedef struct __attribute__((packed)) {
    uint8_t  seq_num;
    uint8_t  new_state;        /**< iot_alarm_state_t */
    uint8_t  reason;           /**< iot_alarm_reason_t */
    uint32_t local_latency_us; /**< Latencia medida desde ISR/cmd hasta LED en Nodo 2 */
    uint8_t  fail_counter;     /**< 0..3 */
    uint8_t  flags;            /**< bit0 = pir_active */
} ble_alarm_event_t;
_Static_assert(sizeof(ble_alarm_event_t) == 9, "ble_alarm_event_t debe medir 9 bytes");

typedef struct __attribute__((packed)) {
    uint8_t seq_num;
    uint8_t uid_len;
    uint8_t uid[IOT_RFID_UID_MAX_LEN];
    uint8_t local_attempt_hint;
} ble_rfid_auth_fail_t;
_Static_assert(sizeof(ble_rfid_auth_fail_t) == 10, "ble_rfid_auth_fail_t debe medir 10 bytes");

typedef struct __attribute__((packed)) {
    uint8_t seq_num;
    uint8_t lock_cmd; /**< 0=CERRAR, 1=ABRIR */
} ble_lock_set_t;
_Static_assert(sizeof(ble_lock_set_t) == 2, "ble_lock_set_t debe medir 2 bytes");

typedef struct __attribute__((packed)) {
    uint8_t seq_num;
    uint8_t lock_state; /**< iot_lock_state_t */
    uint8_t source;     /**< ble_lock_src_t */
    uint8_t uid_len;
    uint8_t uid[IOT_RFID_UID_MAX_LEN];
} ble_lock_status_t;
_Static_assert(sizeof(ble_lock_status_t) == 11, "ble_lock_status_t debe medir 11 bytes");

typedef struct __attribute__((packed)) {
    uint8_t seq_num;
    uint8_t action; /**< ble_rfid_action_t */
    uint8_t uid_len;
    uint8_t uid[IOT_RFID_UID_MAX_LEN];
} ble_rfid_mgmt_t;
_Static_assert(sizeof(ble_rfid_mgmt_t) == 10, "ble_rfid_mgmt_t debe medir 10 bytes");

typedef struct __attribute__((packed)) {
    uint8_t seq_num;
    int16_t temp_deci_c;      /**< ej. 265 = 26.5 C */
    int16_t delta_deci_c;     /**< cambio respecto a lectura previa, ej. +23 = +2.3 C */
    uint8_t humidity_pct;     /**< 0..100 */
    uint8_t alert_state;      /**< iot_temp_alert_t */
    uint8_t threshold_high_c; /**< 15..50 C (defecto 30 C) */
} ble_clima_telemetry_t;
_Static_assert(sizeof(ble_clima_telemetry_t) == 8, "ble_clima_telemetry_t debe medir 8 bytes");

typedef struct __attribute__((packed)) {
    uint8_t seq_num;
    uint8_t new_t_high_c; /**< 15..50 C */
} ble_threshold_set_t;
_Static_assert(sizeof(ble_threshold_set_t) == 2, "ble_threshold_set_t debe medir 2 bytes");

typedef struct __attribute__((packed)) {
    uint8_t  node_role;
    uint32_t uptime_s;
    uint16_t free_heap_kb;
    uint16_t deadline_misses;
    uint8_t  flags; /**< bit0=ble_connected, bit1=sensor_ok */
} ble_heartbeat_t;
_Static_assert(sizeof(ble_heartbeat_t) == 10, "ble_heartbeat_t debe medir 10 bytes");

typedef struct __attribute__((packed)) {
    uint8_t req_flags; /**< 0x01=state, 0x02=str_metrics, 0x03=both */
} ble_status_req_t;
_Static_assert(sizeof(ble_status_req_t) == 1, "ble_status_req_t debe medir 1 byte");

typedef struct __attribute__((packed)) {
    uint8_t  node_role;
    uint32_t max_crit_latency_us;
    uint32_t avg_jitter_us;
    uint16_t deadline_misses;
    uint16_t min_stack_free_bytes;
    uint8_t  cpu_busy_pct;
    uint8_t  last_rx_hops;
} ble_metrics_report_t;
_Static_assert(sizeof(ble_metrics_report_t) == 15, "ble_metrics_report_t debe medir 15 bytes");

typedef struct __attribute__((packed)) {
    uint8_t ack_opcode;
    uint8_t seq_num;
    uint8_t status; /**< 0=OK, 1=ERR_INVALID */
} ble_proto_ack_t;
_Static_assert(sizeof(ble_proto_ack_t) == 3, "ble_proto_ack_t debe medir 3 bytes");

/* ============================================================================
 * Deduplicación e idempotencia
 * ============================================================================ */
#define BLE_DEDUP_SLOTS 16U

typedef struct {
    uint16_t src_addr;
    uint8_t  opcode;
    uint8_t  seq_num;
    bool     used;
} ble_dedup_entry_t;

typedef struct {
    ble_dedup_entry_t entries[BLE_DEDUP_SLOTS];
    uint8_t next_slot;
} ble_dedup_table_t;

/**
 * @brief Retorna true si la pareja (src_addr, opcode, seq_num) es nueva y la registra;
 *        false si es duplicada.
 */
bool ble_dedup_check(ble_dedup_table_t *tbl, uint16_t src_addr, uint8_t opcode, uint8_t seq_num);

#ifdef __cplusplus
}
#endif
