#include "star_coordinator.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "ble_central_star.h"
#include "status_led.h"
#include "telegram_bot.h"

static const char *TAG = "star_coord";

#define S3_ROUTE_DEADLINE_US      100000U    /* D = 100 ms segun diseno_rtos_str.md */
#define S3_OFFLINE_TIMEOUT_US     45000000LL /* 45 s (3 heartbeats perdidos) */
#define S3_STATUS_SUPPRESS_US     1500000LL  /* Ventana para suprimir spam tras OP_STATUS_REQ */

typedef enum {
    COORD_EVT_BLE_RX = 0,
    COORD_EVT_NODE_STATE,
} coord_evt_type_t;

typedef struct {
    coord_evt_type_t type;
    iot_node_role_t role;
    uint8_t opcode;
    uint8_t len;
    bool ready;
    int64_t ts_us;
    uint8_t payload[BLE_PROTO_MAX_FRAME_LEN];
} coord_evt_t;

typedef struct {
    bool connected;
    bool offline_warned;
    int64_t last_seen_us;
    ble_heartbeat_t hb;
    ble_metrics_report_t metrics;
    bool has_hb;
    bool has_metrics;
} node_link_state_t;

typedef struct {
    node_link_state_t links[3]; /* 0: N1, 1: N2, 2: N3 */
    iot_lock_state_t lock_state;
    iot_alarm_state_t alarm_state;
    uint8_t alarm_fail_cnt;
    bool pir_active;
    bool alarm_synced;
    bool lock_synced;
    int16_t temp_deci_c;
    int16_t delta_deci_c;
    uint8_t humidity_pct;
    iot_temp_alert_t temp_alert;
    uint8_t t_high_c;
    bool clima_valid;
    bool wifi_online;
    int64_t last_status_req_us;
} star_global_state_t;

static str_metrics_ctx_t *s_metrics = NULL;
static QueueHandle_t s_coord_queue = NULL;
static SemaphoreHandle_t s_state_mutex = NULL;
static TaskHandle_t s_fsm_task = NULL;
static ble_dedup_table_t s_dedup = { 0 };
static uint8_t s_tx_seq = 0;

static star_global_state_t s_global = {
    .lock_state = LOCK_STATE_CERRADA,
    .alarm_state = ALARM_STATE_DESARMADA,
    .temp_alert = TEMP_ALERT_NORMAL,
    .t_high_c = IOT_TEMP_THRESH_DEFAULT_C,
};

static int role_idx(iot_node_role_t role)
{
    if (role < NODE_ROLE_ACCESO || role > NODE_ROLE_CLIMA) {
        return -1;
    }
    return (int)role - 1;
}

static const char *role_adv_name(int idx)
{
    switch (idx) {
    case 0:  return BLE_ADV_NAME_NODO1;
    case 1:  return BLE_ADV_NAME_NODO2;
    case 2:  return BLE_ADV_NAME_NODO3;
    default: return "DESCONOCIDO";
    }
}

static uint16_t role_src_addr(iot_node_role_t role)
{
    switch (role) {
    case NODE_ROLE_ACCESO:    return BLE_NODE_ID_NODO1_ACCESO;
    case NODE_ROLE_SEGURIDAD: return BLE_NODE_ID_NODO2_SEGURIDAD;
    case NODE_ROLE_CLIMA:     return BLE_NODE_ID_NODO3_CLIMA;
    default:                  return BLE_NODE_ID_GATEWAY_S3;
    }
}

static const char *alarm_reason_str(uint8_t reason)
{
    switch ((iot_alarm_reason_t)reason) {
    case ALARM_REASON_CMD_TG:       return "Telegram";
    case ALARM_REASON_RFID_OPEN:    return "RFID apertura";
    case ALARM_REASON_RFID_CLOSE:   return "RFID cierre";
    case ALARM_REASON_PIR_MOTION:   return "Movimiento PIR";
    case ALARM_REASON_RFID_3_FAILS: return "3 intentos RFID fallidos";
    default:                        return "Desconocido";
    }
}

static uint8_t next_tx_seq(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const uint8_t seq = ++s_tx_seq;
    xSemaphoreGive(s_state_mutex);
    return seq;
}

static void refresh_gateway_rgb_locked(void)
{
    if (s_global.alarm_state == ALARM_STATE_DISPARADA ||
        s_global.temp_alert != TEMP_ALERT_NORMAL) {
        status_led_set_state(STATUS_LED_ALARM_TRIGGERED);
        return;
    }

    const uint8_t cnt = ble_central_star_connected_count();
    if (cnt == 0U) {
        status_led_set_state(STATUS_LED_DISCONNECTED);
    } else if (cnt < 3U) {
        status_led_set_state(STATUS_LED_CONNECTING);
    } else if (s_global.wifi_online) {
        status_led_set_state(STATUS_LED_ONLINE);
    } else {
        status_led_set_state(STATUS_LED_WIFI_OFFLINE);
    }
}

bool star_parse_uid_hex(const char *hex, uint8_t *out_uid, uint8_t *out_len)
{
    if (!hex || !out_uid || !out_len) {
        return false;
    }

    uint8_t buf[IOT_RFID_UID_MAX_LEN] = { 0 };
    uint8_t idx = 0;
    const char *p = hex;

    while (*p != '\0') {
        while (*p == ' ' || *p == ':' || *p == '-') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1]) || idx >= IOT_RFID_UID_MAX_LEN) {
            return false;
        }
        char byte_str[3] = { p[0], p[1], '\0' };
        buf[idx++] = (uint8_t)strtoul(byte_str, NULL, 16);
        p += 2;
    }

    if (idx != 4U && idx != 7U) {
        return false;
    }
    memcpy(out_uid, buf, idx);
    *out_len = idx;
    return true;
}

void star_format_uid_hex(const uint8_t *uid, uint8_t len, char *out, size_t out_sz)
{
    if (!out || out_sz == 0U) {
        return;
    }
    out[0] = '\0';
    if (!uid || len == 0U) {
        return;
    }
    size_t pos = 0;
    const size_t n = (len > IOT_RFID_UID_MAX_LEN) ? IOT_RFID_UID_MAX_LEN : len;
    for (size_t i = 0; i < n && pos + 4U < out_sz; i++) {
        pos += (size_t)snprintf(out + pos, out_sz - pos, (i + 1U < n) ? "%02X:" : "%02X", uid[i]);
    }
}

static void handle_rx_event(const coord_evt_t *evt)
{
    const int idx = role_idx(evt->role);
    if (idx < 0) {
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const bool was_offline_warned = s_global.links[idx].offline_warned;
    s_global.links[idx].last_seen_us = evt->ts_us;
    s_global.links[idx].offline_warned = false;
    xSemaphoreGive(s_state_mutex);

    if (was_offline_warned) {
        char msg[128];
        snprintf(msg, sizeof(msg), "📡 Nodo N%d (%s) recupero heartbeat.", idx + 1, role_adv_name(idx));
        (void)telegram_bot_send(TG_PRIO_NORMAL, msg);
    }

    const uint16_t src_addr = role_src_addr(evt->role);

    switch (evt->opcode) {
    case OP_ALARM_SET:
        if (evt->role == NODE_ROLE_ACCESO && evt->len == sizeof(ble_alarm_set_t)) {
            const ble_alarm_set_t *pkt = (const ble_alarm_set_t *)evt->payload;
            if (!ble_dedup_check(&s_dedup, src_addr, OP_ALARM_SET, pkt->seq_num)) {
                return;
            }
            const esp_err_t err = ble_central_star_send_cmd(NODE_ROLE_SEGURIDAD, OP_ALARM_SET, pkt, sizeof(*pkt));
            const uint32_t lat_us = (uint32_t)(esp_timer_get_time() - evt->ts_us);
            str_metrics_record(s_metrics, "s3_route_n1_to_n2_us", lat_us, S3_ROUTE_DEADLINE_US, false);
            ESP_LOGI(TAG, "RUTA N1->S3->N2 OP_ALARM_SET(%s) seq=%u -> %s (%" PRIu32 " us)",
                     iot_alarm_state_str((iot_alarm_state_t)pkt->target_state),
                     pkt->seq_num,
                     (err == ESP_OK) ? "ENVIADO" : "N2_OFFLINE",
                     lat_us);
        }
        break;

    case OP_RFID_AUTH_FAIL:
        if (evt->role == NODE_ROLE_ACCESO && evt->len == sizeof(ble_rfid_auth_fail_t)) {
            const ble_rfid_auth_fail_t *pkt = (const ble_rfid_auth_fail_t *)evt->payload;
            if (!ble_dedup_check(&s_dedup, src_addr, OP_RFID_AUTH_FAIL, pkt->seq_num)) {
                return;
            }
            char uid_buf[24];
            star_format_uid_hex(pkt->uid, pkt->uid_len, uid_buf, sizeof(uid_buf));
            const esp_err_t err = ble_central_star_send_cmd(NODE_ROLE_SEGURIDAD, OP_RFID_AUTH_FAIL, pkt, sizeof(*pkt));
            const uint32_t lat_us = (uint32_t)(esp_timer_get_time() - evt->ts_us);
            str_metrics_record(s_metrics, "s3_route_n1_to_n2_us", lat_us, S3_ROUTE_DEADLINE_US, false);

            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            const iot_alarm_state_t cur_alarm = s_global.alarm_state;
            xSemaphoreGive(s_state_mutex);

            ESP_LOGW(TAG, "RUTA N1->S3->N2 OP_RFID_AUTH_FAIL [%s] seq=%u -> %s",
                     uid_buf[0] ? uid_buf : "--",
                     pkt->seq_num,
                     (err == ESP_OK) ? "ENVIADO" : "N2_OFFLINE");

            char msg[160];
            snprintf(msg, sizeof(msg),
                     "⚠️ Tarjeta RFID rechazada: %s%s",
                     uid_buf[0] ? uid_buf : "--",
                     (err == ESP_OK) ? "" : " (Nodo 2 offline)");
            (void)telegram_bot_send((cur_alarm == ALARM_STATE_ARMADA) ? TG_PRIO_URGENT : TG_PRIO_NORMAL, msg);
        }
        break;

    case OP_LOCK_STATUS:
        if (evt->len == sizeof(ble_lock_status_t)) {
            const ble_lock_status_t *pkt = (const ble_lock_status_t *)evt->payload;
            if (!ble_dedup_check(&s_dedup, src_addr, OP_LOCK_STATUS, pkt->seq_num)) {
                return;
            }

            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            const bool was_synced = s_global.lock_synced;
            const iot_lock_state_t prev_lock = s_global.lock_state;
            const iot_lock_state_t new_lock = (iot_lock_state_t)pkt->lock_state;
            s_global.lock_state = new_lock;
            s_global.lock_synced = true;
            xSemaphoreGive(s_state_mutex);

            char uid_buf[24];
            star_format_uid_hex(pkt->uid, pkt->uid_len, uid_buf, sizeof(uid_buf));
            ESP_LOGI(TAG, "N1 CERRADURA=%s src=%u uid=%s",
                     iot_lock_state_str(new_lock),
                     pkt->source,
                     uid_buf[0] ? uid_buf : "-");

            if (was_synced && new_lock != prev_lock) {
                char msg[160];
                if (pkt->source == BLE_LOCK_SRC_RFID_VALID && uid_buf[0] != '\0') {
                    snprintf(msg, sizeof(msg), "%s Cerradura %s (RFID %s)",
                             (new_lock == LOCK_STATE_ABIERTA) ? "🔑" : "🔒",
                             iot_lock_state_str(new_lock),
                             uid_buf);
                } else {
                    snprintf(msg, sizeof(msg), "%s Cerradura %s (Telegram)",
                             (new_lock == LOCK_STATE_ABIERTA) ? "🔑" : "🔒",
                             iot_lock_state_str(new_lock));
                }
                (void)telegram_bot_send(TG_PRIO_NORMAL, msg);
            }
        }
        break;

    case OP_RFID_MGMT_ACK:
        if (evt->len >= 4U && evt->len <= sizeof(ble_rfid_mgmt_ack_t)) {
            const ble_rfid_mgmt_ack_t *ack = (const ble_rfid_mgmt_ack_t *)evt->payload;
            ESP_LOGI(TAG, "N1 RFID_MGMT_ACK action=%u status=%u tarjetas=%u",
                     ack->action, ack->status, ack->count);

            const char *act_str = "LIST";
            if (ack->action == BLE_RFID_ACT_ADD) {
                act_str = "ADD";
            } else if (ack->action == BLE_RFID_ACT_DEL) {
                act_str = "DEL";
            }

            const char *st_str = "OK";
            if (ack->status == 1U) {
                st_str = "ERR_INVALIDA";
            } else if (ack->status == 2U) {
                st_str = "ERR_MEMORIA_LLENA";
            } else if (ack->status == 3U) {
                st_str = "ERR_NO_ENCONTRADA";
            }

            char msg[512];
            int pos = snprintf(msg, sizeof(msg),
                               "🪪 RFID %s -> %s (%u/%u tarjetas):",
                               act_str, st_str,
                               (unsigned int)ack->count,
                               (unsigned int)IOT_RFID_MAX_CARDS);
            for (uint8_t i = 0; i < ack->count && i < IOT_RFID_MAX_CARDS && pos > 0 && (size_t)pos < sizeof(msg) - 32U; i++) {
                char uid_buf[24];
                star_format_uid_hex(ack->cards[i].uid, ack->cards[i].uid_len, uid_buf, sizeof(uid_buf));
                ESP_LOGI(TAG, "  [%u] %s", (unsigned int)i + 1U, uid_buf);
                pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                                "\n%u. %s", (unsigned int)i + 1U, uid_buf[0] ? uid_buf : "--");
            }
            (void)telegram_bot_send(TG_PRIO_NORMAL, msg);
        }
        break;

    case OP_ALARM_EVENT:
        if (evt->len == sizeof(ble_alarm_event_t)) {
            const ble_alarm_event_t *pkt = (const ble_alarm_event_t *)evt->payload;
            if (!ble_dedup_check(&s_dedup, src_addr, OP_ALARM_EVENT, pkt->seq_num)) {
                return;
            }

            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            const bool was_synced = s_global.alarm_synced;
            const iot_alarm_state_t prev_state = s_global.alarm_state;
            const iot_alarm_state_t new_state = (iot_alarm_state_t)pkt->new_state;
            s_global.alarm_state = new_state;
            s_global.alarm_fail_cnt = pkt->fail_counter;
            s_global.pir_active = (pkt->flags & 0x01U) != 0U;
            s_global.alarm_synced = true;
            refresh_gateway_rgb_locked();
            xSemaphoreGive(s_state_mutex);

            ESP_LOGW(TAG, "N2 ALARMA=%s reason=%u fail=%u/3 lat_local=%" PRIu32 " us",
                     iot_alarm_state_str(new_state),
                     pkt->reason, pkt->fail_counter, pkt->local_latency_us);

            char msg[224];
            if (new_state == ALARM_STATE_DISPARADA) {
                snprintf(msg, sizeof(msg),
                         "🚨 ALARMA DISPARADA\nMotivo: %s\nFallos RFID: %u/3\nLatencia local N2: %" PRIu32 " us\nUsa /desarmar o tarjeta RFID valida.",
                         alarm_reason_str(pkt->reason),
                         pkt->fail_counter,
                         pkt->local_latency_us);
                (void)telegram_bot_send(TG_PRIO_URGENT, msg);
            } else if (was_synced && new_state != prev_state) {
                if (new_state == ALARM_STATE_ARMADA) {
                    snprintf(msg, sizeof(msg), "🛡️ Alarma ARMADA (%s)", alarm_reason_str(pkt->reason));
                } else if (prev_state == ALARM_STATE_DISPARADA) {
                    snprintf(msg, sizeof(msg), "✅ Alarma silenciada y DESARMADA (%s)", alarm_reason_str(pkt->reason));
                } else {
                    snprintf(msg, sizeof(msg), "🔓 Alarma DESARMADA (%s)", alarm_reason_str(pkt->reason));
                }
                (void)telegram_bot_send(TG_PRIO_NORMAL, msg);
            }
        }
        break;

    case OP_CLIMA_TELEMETRY:
        if (evt->len == sizeof(ble_clima_telemetry_t)) {
            const ble_clima_telemetry_t *pkt = (const ble_clima_telemetry_t *)evt->payload;
            if (!ble_dedup_check(&s_dedup, src_addr, OP_CLIMA_TELEMETRY, pkt->seq_num)) {
                return;
            }

            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            const bool prev_valid = s_global.clima_valid;
            const iot_temp_alert_t prev_alert = s_global.temp_alert;
            const int64_t last_req_us = s_global.last_status_req_us;
            const iot_temp_alert_t new_alert = (iot_temp_alert_t)pkt->alert_state;

            s_global.temp_deci_c = pkt->temp_deci_c;
            s_global.delta_deci_c = pkt->delta_deci_c;
            s_global.humidity_pct = pkt->humidity_pct;
            s_global.temp_alert = new_alert;
            s_global.t_high_c = pkt->threshold_high_c;
            s_global.clima_valid = true;
            refresh_gateway_rgb_locked();
            xSemaphoreGive(s_state_mutex);

            ESP_LOGI(TAG, "N3 CLIMA T=%.1fC (dT=%+.1fC) H=%u%% alerta=%s T_high=%uC",
                     pkt->temp_deci_c / 10.0,
                     pkt->delta_deci_c / 10.0,
                     pkt->humidity_pct,
                     iot_temp_alert_str(new_alert),
                     pkt->threshold_high_c);

            const bool from_status_req = (last_req_us > 0) && ((evt->ts_us - last_req_us) < S3_STATUS_SUPPRESS_US);
            if (from_status_req && new_alert == prev_alert) {
                break;
            }

            char msg[224];
            if (new_alert == TEMP_ALERT_SPIKE || new_alert == TEMP_ALERT_BOTH) {
                snprintf(msg, sizeof(msg),
                         "🌡️⚠️ Sobresalto termico detectado (%s)\nT=%.1f °C (dT=%+.1f °C) | H=%u%% | Umbral=%u °C",
                         iot_temp_alert_str(new_alert),
                         pkt->temp_deci_c / 10.0,
                         pkt->delta_deci_c / 10.0,
                         pkt->humidity_pct,
                         pkt->threshold_high_c);
                (void)telegram_bot_send(TG_PRIO_URGENT, msg);
            } else if (new_alert == TEMP_ALERT_HIGH && (!prev_valid || prev_alert != TEMP_ALERT_HIGH)) {
                snprintf(msg, sizeof(msg),
                         "🔥 Temperatura alta: T=%.1f °C >= %u °C (H=%u%%)",
                         pkt->temp_deci_c / 10.0,
                         pkt->threshold_high_c,
                         pkt->humidity_pct);
                (void)telegram_bot_send(TG_PRIO_URGENT, msg);
            } else if (prev_valid && prev_alert != TEMP_ALERT_NORMAL && new_alert == TEMP_ALERT_NORMAL) {
                snprintf(msg, sizeof(msg),
                         "✅ Temperatura normalizada: T=%.1f °C (dT=%+.1f °C, H=%u%%, Umbral=%u °C)",
                         pkt->temp_deci_c / 10.0,
                         pkt->delta_deci_c / 10.0,
                         pkt->humidity_pct,
                         pkt->threshold_high_c);
                (void)telegram_bot_send(TG_PRIO_NORMAL, msg);
            }
        }
        break;

    case OP_HEARTBEAT:
        if (evt->len == sizeof(ble_heartbeat_t)) {
            const ble_heartbeat_t *hb = (const ble_heartbeat_t *)evt->payload;
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_global.links[idx].hb = *hb;
            s_global.links[idx].has_hb = true;
            xSemaphoreGive(s_state_mutex);
        }
        break;

    case OP_METRICS_REPORT:
        if (evt->len == sizeof(ble_metrics_report_t)) {
            const ble_metrics_report_t *rep = (const ble_metrics_report_t *)evt->payload;
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_global.links[idx].metrics = *rep;
            s_global.links[idx].has_metrics = true;
            xSemaphoreGive(s_state_mutex);

            ESP_LOGI(TAG, "METRICAS N%u: max_lat=%" PRIu32 "us avg_jit=%" PRIu32 "us misses=%u min_stack=%uB",
                     rep->node_role, rep->max_crit_latency_us, rep->avg_jitter_us,
                     rep->deadline_misses, rep->min_stack_free_bytes);
        }
        break;

    case OP_PROTO_ACK:
        if (evt->len == sizeof(ble_proto_ack_t)) {
            const ble_proto_ack_t *ack = (const ble_proto_ack_t *)evt->payload;
            ESP_LOGI(TAG, "ACK desde N%d: opcode=0x%02X seq=%u status=%u",
                     idx + 1, ack->ack_opcode, ack->seq_num, ack->status);
        }
        break;

    default:
        ESP_LOGW(TAG, "Opcode BLE inesperado desde N%d: 0x%02X len=%u",
                 idx + 1, evt->opcode, evt->len);
        break;
    }
}

static void task_state_fsm_s3(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);

    coord_evt_t evt;
    while (1) {
        if (xQueueReceive(s_coord_queue, &evt, pdMS_TO_TICKS(2000)) == pdTRUE) {
            if (evt.type == COORD_EVT_BLE_RX) {
                handle_rx_event(&evt);
            } else if (evt.type == COORD_EVT_NODE_STATE) {
                const int idx = role_idx(evt.role);
                if (idx >= 0) {
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    s_global.links[idx].connected = evt.ready;
                    s_global.links[idx].offline_warned = false;
                    if (evt.ready) {
                        s_global.links[idx].last_seen_us = evt.ts_us;
                        s_global.last_status_req_us = evt.ts_us;
                    }
                    refresh_gateway_rgb_locked();
                    xSemaphoreGive(s_state_mutex);

                    const uint8_t cnt = ble_central_star_connected_count();
                    ESP_LOGI(TAG, "Nodo N%d (%s) -> %s (conectados=%u/3)",
                             idx + 1, role_adv_name(idx),
                             evt.ready ? "ONLINE" : "DESCONECTADO", cnt);

                    char msg[128];
                    snprintf(msg, sizeof(msg), "%s Nodo N%d (%s) -> %s (%u/3 BLE)",
                             evt.ready ? "🟢" : "🔴",
                             idx + 1,
                             role_adv_name(idx),
                             evt.ready ? "ONLINE" : "DESCONECTADO",
                             cnt);
                    (void)telegram_bot_send(TG_PRIO_NORMAL, msg);

                    if (evt.ready) {
                        const ble_status_req_t req = { .req_flags = 0x01U };
                        (void)ble_central_star_send_cmd(evt.role, OP_STATUS_REQ, &req, sizeof(req));
                    }
                }
            }
        }

        const int64_t now = esp_timer_get_time();
        bool warn_nodes[3] = { false, false, false };
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        for (int i = 0; i < 3; i++) {
            if (s_global.links[i].connected &&
                !s_global.links[i].offline_warned &&
                (now - s_global.links[i].last_seen_us) > S3_OFFLINE_TIMEOUT_US) {
                s_global.links[i].offline_warned = true;
                warn_nodes[i] = true;
                ESP_LOGW(TAG, "Timeout heartbeat > 45s en Nodo N%d", i + 1);
            }
        }
        xSemaphoreGive(s_state_mutex);

        for (int i = 0; i < 3; i++) {
            if (warn_nodes[i]) {
                char msg[128];
                snprintf(msg, sizeof(msg), "⚠️ Nodo N%d (%s) sin heartbeat (>45 s).",
                         i + 1, role_adv_name(i));
                (void)telegram_bot_send(TG_PRIO_NORMAL, msg);
            }
        }

        esp_task_wdt_reset();
    }
}

esp_err_t star_coordinator_init(str_metrics_ctx_t *metrics, TaskHandle_t *out_fsm_task)
{
    s_metrics = metrics;
    s_state_mutex = xSemaphoreCreateMutex();
    s_coord_queue = xQueueCreate(16, sizeof(coord_evt_t));
    if (!s_state_mutex || !s_coord_queue) {
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t ok = xTaskCreatePinnedToCore(task_state_fsm_s3,
                                                  "task_state_fsm_s3",
                                                  4096,
                                                  NULL,
                                                  12,
                                                  &s_fsm_task,
                                                  1);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (out_fsm_task) {
        *out_fsm_task = s_fsm_task;
    }
    return ESP_OK;
}

void star_coordinator_on_ble_rx(iot_node_role_t role,
                                uint8_t opcode,
                                const uint8_t *payload,
                                size_t len,
                                int64_t rx_ts_us,
                                void *ctx)
{
    (void)ctx;
    if (!s_coord_queue || len > BLE_PROTO_MAX_FRAME_LEN) {
        return;
    }

    coord_evt_t evt = {
        .type = COORD_EVT_BLE_RX,
        .role = role,
        .opcode = opcode,
        .len = (uint8_t)len,
        .ts_us = rx_ts_us,
    };
    if (len > 0U && payload) {
        memcpy(evt.payload, payload, len);
    }
    (void)xQueueSend(s_coord_queue, &evt, 0);
}

void star_coordinator_on_node_state(iot_node_role_t role, bool ready, void *ctx)
{
    (void)ctx;
    if (!s_coord_queue) {
        return;
    }

    const coord_evt_t evt = {
        .type = COORD_EVT_NODE_STATE,
        .role = role,
        .ready = ready,
        .ts_us = esp_timer_get_time(),
    };
    (void)xQueueSend(s_coord_queue, &evt, 0);
}

void star_coordinator_set_wifi_online(bool online)
{
    if (!s_state_mutex) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_global.wifi_online = online;
    refresh_gateway_rgb_locked();
    xSemaphoreGive(s_state_mutex);
}

void star_coordinator_get_snapshot(star_global_snapshot_t *out)
{
    if (!out) {
        return;
    }
    if (!s_state_mutex) {
        memset(out, 0, sizeof(*out));
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    for (int i = 0; i < 3; i++) {
        out->nodes[i].connected = s_global.links[i].connected;
        out->nodes[i].offline_warned = s_global.links[i].offline_warned;
        out->nodes[i].hb = s_global.links[i].hb;
        out->nodes[i].metrics = s_global.links[i].metrics;
        out->nodes[i].has_hb = s_global.links[i].has_hb;
        out->nodes[i].has_metrics = s_global.links[i].has_metrics;
    }
    out->lock_state = s_global.lock_state;
    out->alarm_state = s_global.alarm_state;
    out->alarm_fail_cnt = s_global.alarm_fail_cnt;
    out->pir_active = s_global.pir_active;
    out->temp_deci_c = s_global.temp_deci_c;
    out->delta_deci_c = s_global.delta_deci_c;
    out->humidity_pct = s_global.humidity_pct;
    out->temp_alert = s_global.temp_alert;
    out->t_high_c = s_global.t_high_c;
    out->clima_valid = s_global.clima_valid;
    out->wifi_online = s_global.wifi_online;
    xSemaphoreGive(s_state_mutex);
}

esp_err_t star_cmd_alarm(iot_alarm_state_t target)
{
    const ble_alarm_set_t pkt = {
        .seq_num = next_tx_seq(),
        .target_state = (uint8_t)target,
        .source = BLE_SRC_TELEGRAM,
    };
    return ble_central_star_send_cmd(NODE_ROLE_SEGURIDAD, OP_ALARM_SET, &pkt, sizeof(pkt));
}

esp_err_t star_cmd_lock(iot_lock_state_t target)
{
    const ble_lock_set_t pkt = {
        .seq_num = next_tx_seq(),
        .lock_cmd = (target == LOCK_STATE_ABIERTA) ? 1U : 0U,
    };
    return ble_central_star_send_cmd(NODE_ROLE_ACCESO, OP_LOCK_SET, &pkt, sizeof(pkt));
}

esp_err_t star_cmd_rfid(ble_rfid_action_t action, const uint8_t *uid, uint8_t uid_len)
{
    ble_rfid_mgmt_t pkt = {
        .seq_num = next_tx_seq(),
        .action = (uint8_t)action,
        .uid_len = uid_len,
    };
    if (uid && uid_len > 0U) {
        const size_t n = (uid_len > IOT_RFID_UID_MAX_LEN) ? IOT_RFID_UID_MAX_LEN : uid_len;
        memcpy(pkt.uid, uid, n);
    }
    return ble_central_star_send_cmd(NODE_ROLE_ACCESO, OP_RFID_MGMT, &pkt, sizeof(pkt));
}

esp_err_t star_cmd_threshold(uint8_t new_t_high_c)
{
    if (new_t_high_c < IOT_TEMP_THRESH_MIN_C || new_t_high_c > IOT_TEMP_THRESH_MAX_C) {
        return ESP_ERR_INVALID_ARG;
    }
    const ble_threshold_set_t pkt = {
        .seq_num = next_tx_seq(),
        .new_t_high_c = new_t_high_c,
    };
    return ble_central_star_send_cmd(NODE_ROLE_CLIMA, OP_THRESHOLD_SET, &pkt, sizeof(pkt));
}

void star_cmd_request_status_all(uint8_t req_flags)
{
    if (s_state_mutex) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_global.last_status_req_us = esp_timer_get_time();
        xSemaphoreGive(s_state_mutex);
    }
    const ble_status_req_t req = { .req_flags = req_flags };
    const iot_node_role_t roles[] = { NODE_ROLE_ACCESO, NODE_ROLE_SEGURIDAD, NODE_ROLE_CLIMA };
    for (size_t i = 0; i < 3U; i++) {
        (void)ble_central_star_send_cmd(roles[i], OP_STATUS_REQ, &req, sizeof(req));
    }
}

void star_log_global_status(void)
{
    star_global_snapshot_t st;
    star_coordinator_get_snapshot(&st);

    ESP_LOGI(TAG,
             "ESTADO GLOBAL | Cerradura=%s | Alarma=%s (fallos=%u/3) | Clima=%s T=%.1fC (dT=%+.1fC) H=%u%% alerta=%s T_high=%uC | BLE=%u/3 | WiFi=%s",
             iot_lock_state_str(st.lock_state),
             iot_alarm_state_str(st.alarm_state),
             st.alarm_fail_cnt,
             st.clima_valid ? "OK" : "SIN_DATO",
             st.temp_deci_c / 10.0,
             st.delta_deci_c / 10.0,
             st.humidity_pct,
             iot_temp_alert_str(st.temp_alert),
             st.t_high_c,
             ble_central_star_connected_count(),
             st.wifi_online ? "ONLINE" : "OFFLINE");
}

void star_log_ble_network(void)
{
    const iot_node_role_t roles[] = { NODE_ROLE_ACCESO, NODE_ROLE_SEGURIDAD, NODE_ROLE_CLIMA };
    ESP_LOGI(TAG, "=== Red BLE GATT en Estrella (%u/3 conectados) ===",
             ble_central_star_connected_count());
    for (size_t i = 0; i < 3U; i++) {
        ble_central_node_info_t info;
        if (ble_central_star_get_info(roles[i], &info)) {
            ESP_LOGI(TAG, "  N%u %-16s : %s conn=%u rssi=%d dBm uptime=%" PRIu32 "s",
                     info.role, info.name,
                     info.ready ? "READY" : (info.connecting ? "CONNECTING" : "DISCONNECTED"),
                     info.conn_handle, info.rssi, info.uptime_s);
        }
    }
}
