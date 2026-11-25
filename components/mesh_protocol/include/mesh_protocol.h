#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "iot_common_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Identificación y direccionamiento BLE Mesh
 * ============================================================================ */
#define MESH_PROJECT_MAGIC_0      0xAAU
#define MESH_PROJECT_MAGIC_1      0x55U
#define MESH_PROTO_VERSION        0x01U
#define MESH_TX_TTL_DEFAULT       5U

#define MESH_ADDR_GATEWAY_S3      0x0001U
#define MESH_ADDR_NODO1_ACCESO    0x0010U
#define MESH_ADDR_NODO2_SEGURIDAD 0x0020U
#define MESH_ADDR_NODO3_CLIMA     0x0030U

#define MESH_GRP_SYSTEM           0xC000U
#define MESH_GRP_SECURITY         0xC001U
#define MESH_GRP_ACCESS           0xC002U
#define MESH_GRP_CLIMA            0xC003U

#define MESH_VENDOR_CID           0x02E5U
#define MESH_VENDOR_MOD_SRV_ID    0x0001U
#define MESH_VENDOR_MOD_CLI_ID    0x0002U

/* ============================================================================
 * Opcodes del Vendor Model (primer octeto de opcode de 3 bytes con CID 0x02E5)
 * ============================================================================ */
#define OP_ALARM_SET              0xC0U
#define OP_ALARM_EVENT            0xC1U
#define OP_RFID_AUTH_FAIL         0xC2U
#define OP_LOCK_SET               0xC3U
#define OP_LOCK_STATUS            0xC4U
#define OP_RFID_MGMT              0xC5U
#define OP_RFID_MGMT_ACK          0xC6U
#define OP_CLIMA_TELEMETRY        0xC7U
#define OP_FAN_SET                0xC8U
#define OP_THRESHOLD_SET          0xC9U
#define OP_HEARTBEAT              0xCAU
#define OP_STATUS_REQ             0xCBU
#define OP_METRICS_REPORT         0xCCU
#define OP_DEMO_HOP_CTRL          0xCDU
#define OP_VENDOR_ACK             0xCEU

typedef enum {
    MESH_SRC_TELEGRAM        = 1,
    MESH_SRC_RFID_C6_DIRECT  = 2,
} mesh_alarm_cmd_source_t;

typedef enum {
    MESH_LOCK_SRC_TELEGRAM   = 0,
    MESH_LOCK_SRC_RFID_VALID = 1,
} mesh_lock_src_t;

typedef enum {
    MESH_RFID_ACT_LIST = 0,
    MESH_RFID_ACT_ADD  = 1,
    MESH_RFID_ACT_DEL  = 2,
} mesh_rfid_action_t;

typedef enum {
    MESH_CLIMA_REASON_PERIODIC      = 0,
    MESH_CLIMA_REASON_LEVEL_CHANGED = 1,
    MESH_CLIMA_REASON_SENSOR_ERR    = 2,
} mesh_clima_reason_t;

/* ============================================================================
 * Payloads binarios empaquetados (little-endian) según protocolo_ble_mesh.md
 * ============================================================================ */

typedef struct __attribute__((packed)) {
    uint8_t seq_num;
    uint8_t target_state; /**< iot_alarm_state_t (0=DESARMADA, 1=ARMADA) */
    uint8_t source;       /**< mesh_alarm_cmd_source_t */
} mesh_alarm_set_t;
_Static_assert(sizeof(mesh_alarm_set_t) == 3, "mesh_alarm_set_t debe medir 3 bytes");

typedef struct __attribute__((packed)) {
    uint8_t  seq_num;
    uint8_t  new_state;        /**< iot_alarm_state_t */
    uint8_t  reason;           /**< iot_alarm_reason_t */
    uint32_t local_latency_us; /**< Latencia medida desde ISR/cmd hasta LED en N2 */
    uint8_t  fail_counter;     /**< 0..3 */
    uint8_t  flags;            /**< bit0 = pir_active */
} mesh_alarm_event_t;
_Static_assert(sizeof(mesh_alarm_event_t) == 9, "mesh_alarm_event_t debe medir 9 bytes");

typedef struct __attribute__((packed)) {
    uint8_t seq_num;
    uint8_t uid_len;
    uint8_t uid[IOT_RFID_UID_MAX_LEN];
    uint8_t local_attempt_hint;
} mesh_rfid_auth_fail_t;
_Static_assert(sizeof(mesh_rfid_auth_fail_t) == 10, "mesh_rfid_auth_fail_t debe medir 10 bytes");

typedef struct __attribute__((packed)) {
    uint8_t seq_num;
    uint8_t lock_cmd; /**< 0=CERRAR, 1=ABRIR */
} mesh_lock_set_t;
_Static_assert(sizeof(mesh_lock_set_t) == 2, "mesh_lock_set_t debe medir 2 bytes");

typedef struct __attribute__((packed)) {
    uint8_t seq_num;
    uint8_t lock_state; /**< iot_lock_state_t */
    uint8_t source;     /**< mesh_lock_src_t */
    uint8_t uid_len;
    uint8_t uid[IOT_RFID_UID_MAX_LEN];
} mesh_lock_status_t;
_Static_assert(sizeof(mesh_lock_status_t) == 11, "mesh_lock_status_t debe medir 11 bytes");

typedef struct __attribute__((packed)) {
    uint8_t seq_num;
    uint8_t action; /**< mesh_rfid_action_t */
    uint8_t uid_len;
    uint8_t uid[IOT_RFID_UID_MAX_LEN];
} mesh_rfid_mgmt_t;
_Static_assert(sizeof(mesh_rfid_mgmt_t) == 10, "mesh_rfid_mgmt_t debe medir 10 bytes");

typedef struct __attribute__((packed)) {
    uint8_t seq_num;
    int16_t temp_deci_c;      /**< ej. 265 = 26.5 C */
    uint8_t humidity_pct;     /**< 0..100 */
    uint8_t fan_mode;         /**< iot_fan_mode_t */
    uint8_t fan_level;        /**< iot_fan_level_t */
    uint8_t threshold_base_c; /**< 15..40 C */
    uint8_t change_reason;    /**< mesh_clima_reason_t */
} mesh_clima_telemetry_t;
_Static_assert(sizeof(mesh_clima_telemetry_t) == 8, "mesh_clima_telemetry_t debe medir 8 bytes");

typedef struct __attribute__((packed)) {
    uint8_t seq_num;
    uint8_t cmd; /**< 0=OFF, 1=BAJA, 2=MEDIA, 3=ALTA, 4=AUTO */
} mesh_fan_set_t;
_Static_assert(sizeof(mesh_fan_set_t) == 2, "mesh_fan_set_t debe medir 2 bytes");

typedef struct __attribute__((packed)) {
    uint8_t seq_num;
    uint8_t new_t_base_c; /**< 15..40 C */
} mesh_threshold_set_t;
_Static_assert(sizeof(mesh_threshold_set_t) == 2, "mesh_threshold_set_t debe medir 2 bytes");

typedef struct __attribute__((packed)) {
    uint8_t  node_role;
    uint32_t uptime_s;
    uint16_t free_heap_kb;
    uint16_t deadline_misses;
    uint8_t  flags; /**< bit0=demo_hop_active, bit1=sensor_ok */
} mesh_heartbeat_t;
_Static_assert(sizeof(mesh_heartbeat_t) == 10, "mesh_heartbeat_t debe medir 10 bytes");

typedef struct __attribute__((packed)) {
    uint8_t req_flags; /**< 0x01=state, 0x02=str_metrics, 0x03=both */
} mesh_status_req_t;
_Static_assert(sizeof(mesh_status_req_t) == 1, "mesh_status_req_t debe medir 1 byte");

typedef struct __attribute__((packed)) {
    uint8_t  node_role;
    uint32_t max_crit_latency_us;
    uint32_t avg_jitter_us;
    uint16_t deadline_misses;
    uint16_t min_stack_free_bytes;
    uint8_t  cpu_busy_pct;
    uint8_t  last_rx_hops;
} mesh_metrics_report_t;
_Static_assert(sizeof(mesh_metrics_report_t) == 15, "mesh_metrics_report_t debe medir 15 bytes");

typedef struct __attribute__((packed)) {
    uint8_t seq_num;
    uint8_t enable;
    uint8_t target_unicast_low;
} mesh_demo_hop_ctrl_t;
_Static_assert(sizeof(mesh_demo_hop_ctrl_t) == 3, "mesh_demo_hop_ctrl_t debe medir 3 bytes");

typedef struct __attribute__((packed)) {
    uint8_t ack_opcode;
    uint8_t seq_num;
    uint8_t status; /**< 0=OK, 1=ERR_INVALID */
} mesh_vendor_ack_t;
_Static_assert(sizeof(mesh_vendor_ack_t) == 3, "mesh_vendor_ack_t debe medir 3 bytes");

/* ============================================================================
 * Deduplicación e idempotencia
 * ============================================================================ */
#define MESH_DEDUP_SLOTS 16U

typedef struct {
    uint16_t src_addr;
    uint8_t  opcode;
    uint8_t  seq_num;
    bool     used;
} mesh_dedup_entry_t;

typedef struct {
    mesh_dedup_entry_t entries[MESH_DEDUP_SLOTS];
    uint8_t next_slot;
} mesh_dedup_table_t;

void mesh_build_device_uuid(iot_node_role_t role, const uint8_t mac[6], uint8_t out_uuid[16]);

uint8_t mesh_calc_hops(uint8_t tx_ttl, uint8_t rx_ttl);

/**
 * @brief Retorna true si la pareja (src_addr, opcode, seq_num) es nueva y la registra;
 *        false si es duplicada.
 */
bool mesh_dedup_check(mesh_dedup_table_t *tbl, uint16_t src_addr, uint8_t opcode, uint8_t seq_num);

#ifdef __cplusplus
}
#endif
