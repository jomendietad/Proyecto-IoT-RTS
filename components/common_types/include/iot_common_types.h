#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Identificador de rol del nodo en la malla BLE Mesh. */
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

/** @brief Modo de operación del ventilador emulado. */
typedef enum {
    FAN_MODE_MANUAL = 0,
    FAN_MODE_AUTO   = 1,
} iot_fan_mode_t;

/** @brief Niveles PWM discretos del ventilador. */
typedef enum {
    FAN_LEVEL_OFF   = 0,
    FAN_LEVEL_BAJA  = 1,
    FAN_LEVEL_MEDIA = 2,
    FAN_LEVEL_ALTA  = 3,
} iot_fan_level_t;

#define IOT_RFID_UID_MAX_LEN         7U
#define IOT_RFID_MAX_CARDS           8U
#define IOT_RFID_FAIL_TRIGGER_COUNT  3U

#define IOT_FAN_TBASE_DEFAULT_C      25U
#define IOT_FAN_TBASE_MIN_C          15U
#define IOT_FAN_TBASE_MAX_C          40U
#define IOT_FAN_HYSTERESIS_DECI_C    10

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

static inline const char *iot_fan_mode_str(iot_fan_mode_t m)
{
    return (m == FAN_MODE_AUTO) ? "AUTO" : "MANUAL";
}

static inline const char *iot_fan_level_str(iot_fan_level_t l)
{
    switch (l) {
    case FAN_LEVEL_OFF:   return "OFF";
    case FAN_LEVEL_BAJA:  return "BAJA";
    case FAN_LEVEL_MEDIA: return "MEDIA";
    case FAN_LEVEL_ALTA:  return "ALTA";
    default:              return "DESCONOCIDO";
    }
}

#ifdef __cplusplus
}
#endif
