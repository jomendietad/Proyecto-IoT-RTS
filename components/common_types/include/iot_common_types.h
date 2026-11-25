#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Identificador de rol del nodo en la red Bluetooth en estrella. */
typedef enum {
    NODE_ROLE_GATEWAY   = 0,
    NODE_ROLE_ACCESO    = 1,
    NODE_ROLE_SEGURIDAD = 2,
    NODE_ROLE_CLIMA     = 3,
} iot_node_role_t;

/** @brief Estados de la FSM de alarma tipo carro (3 estados exactos). */
typedef enum {
    ALARM_STATE_DESARMADA = 0,
    ALARM_STATE_ARMADA    = 1,
    ALARM_STATE_DISPARADA = 2,
} iot_alarm_state_t;

/** @brief Causa de transición de estado en la alarma. */
typedef enum {
    ALARM_REASON_CMD_TG       = 0,
    ALARM_REASON_RFID_OPEN    = 1,
    ALARM_REASON_RFID_CLOSE   = 2,
    ALARM_REASON_PIR_MOTION   = 3,
    ALARM_REASON_RFID_3_FAILS = 4,
} iot_alarm_reason_t;

/** @brief Estado enclavado (toggle) de la cerradura emulada. */
typedef enum {
    LOCK_STATE_CERRADA = 0,
    LOCK_STATE_ABIERTA = 1,
} iot_lock_state_t;

/** @brief Estado de alerta térmica por sobresalto / umbral en Nodo 3. */
typedef enum {
    TEMP_ALERT_NORMAL = 0,
    TEMP_ALERT_HIGH   = 1, /**< Superó T_high (con histéresis de 1.0 °C al bajar) */
    TEMP_ALERT_SPIKE  = 2, /**< Cambio brusco |ΔT| >= 2.0 °C entre lecturas */
    TEMP_ALERT_BOTH   = 3, /**< Ambos criterios activos simultáneamente */
} iot_temp_alert_t;

#define IOT_RFID_UID_MAX_LEN         7U
#define IOT_RFID_MAX_CARDS           8U
#define IOT_RFID_FAIL_TRIGGER_COUNT  3U

#define IOT_TEMP_THRESH_DEFAULT_C    30U
#define IOT_TEMP_THRESH_MIN_C        15U
#define IOT_TEMP_THRESH_MAX_C        50U
#define IOT_TEMP_HYSTERESIS_DECI_C   10
#define IOT_TEMP_SPIKE_DELTA_DECI_C  20

#define IOT_HEARTBEAT_PERIOD_MS      15000U
#define IOT_TWDT_TIMEOUT_MS          10000U

static inline const char *iot_alarm_state_str(iot_alarm_state_t s)
{
    switch (s) {
    case ALARM_STATE_DESARMADA: return "DESARMADA";
    case ALARM_STATE_ARMADA:    return "ARMADA";
    case ALARM_STATE_DISPARADA: return "DISPARADA";
    default:                    return "DESCONOCIDA";
    }
}

static inline const char *iot_lock_state_str(iot_lock_state_t s)
{
    return (s == LOCK_STATE_ABIERTA) ? "ABIERTA" : "CERRADA";
}

static inline const char *iot_temp_alert_str(iot_temp_alert_t a)
{
    switch (a) {
    case TEMP_ALERT_NORMAL: return "NORMAL";
    case TEMP_ALERT_HIGH:   return "UMBRAL_ALTO";
    case TEMP_ALERT_SPIKE:  return "SOBRESALTO";
    case TEMP_ALERT_BOTH:   return "UMBRAL_Y_SOBRESALTO";
    default:                return "DESCONOCIDA";
    }
}

#ifdef __cplusplus
}
#endif
