#include "star_coordinator.h"

#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "ble_central_star.h"
#include "status_led.h"

static const char *TAG = "star_coord";

#define S3_ROUTE_DEADLINE_US    100000U   /* D = 100 ms según diseno_rtos_str.md */
#define S3_OFFLINE_TIMEOUT_US   45000000LL /* 45 s (3 heartbeats perdidos) */

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
    int16_t temp_deci_c;
    int16_t delta_deci_c;
    uint8_t humidity_pct;
    iot_temp_alert_t temp_alert;
    uint8_t t_high_c;
    bool clima_valid;
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

static uint16_t role_src_addr(iot_node_role_t role)
{
    switch (role) {
    case NODE_ROLE_ACCESO:    return BLE_NODE_ID_NODO1_ACCESO;
    case NODE_ROLE_SEGURIDAD: return BLE_NODE_ID_NODO2_SEGURIDAD;
    case NODE_ROLE_CLIMA:     return BLE_NODE_ID_NODO3_CLIMA;
    default:                  return BLE_NODE_ID_GATEWAY_S3;
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
    } else {
        /* Fase 3 (sin Wi-Fi/Telegram): estrella 3/3 en modo local offline -> Ambar */
        status_led_set_state(STATUS_LED_WIFI_OFFLINE);
    }
}

static void format_uid_hex(const uint8_t *uid, uint8_t len, char *out, size_t out_sz)
{
    if (!out || out_sz == 0U) {
        return;
    }
    out[0] = '\0';
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
    s_global.links[idx].last_seen_us = evt->ts_us;
    s_global.links[idx].offline_warned = false;
    xSemaphoreGive(s_state_mutex);

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
            format_uid_hex(pkt->uid, pkt->uid_len, uid_buf, sizeof(uid_buf));
            const esp_err_t err = ble_central_star_send_cmd(NODE_ROLE_SEGURIDAD, OP_RFID_AUTH_FAIL, pkt, sizeof(*pkt));
            const uint32_t lat_us = (uint32_t)(esp_timer_get_time() - evt->ts_us);
            str_metrics_record(s_metrics, "s3_route_n1_to_n2_us", lat_us, S3_ROUTE_DEADLINE_US, false);
            ESP_LOGW(TAG, "RUTA N1->S3->N2 OP_RFID_AUTH_FAIL [%s] seq=%u -> %s",
                     uid_buf[0] ? uid_buf : "--",
                     pkt->seq_num,
                     (err == ESP_OK) ? "ENVIADO" : "N2_OFFLINE");
        }
        break;

    case OP_LOCK_STATUS:
        if (evt->len == sizeof(ble_lock_status_t)) {
            const ble_lock_status_t *pkt = (const ble_lock_status_t *)evt->payload;
            if (!ble_dedup_check(&s_dedup, src_addr, OP_LOCK_STATUS, pkt->seq_num)) {
                return;
            }
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_global.lock_state = (iot_lock_state_t)pkt->lock_state;
            xSemaphoreGive(s_state_mutex);

            char uid_buf[24];
            format_uid_hex(pkt->uid, pkt->uid_len, uid_buf, sizeof(uid_buf));
            ESP_LOGI(TAG, "N1 CERRADURA=%s src=%u uid=%s",
                     iot_lock_state_str((iot_lock_state_t)pkt->lock_state),
                     pkt->source,
                     uid_buf[0] ? uid_buf : "-");
        }
        break;

    case OP_RFID_MGMT_ACK:
        if (evt->len >= 4U && evt->len <= sizeof(ble_rfid_mgmt_ack_t)) {
            const ble_rfid_mgmt_ack_t *ack = (const ble_rfid_mgmt_ack_t *)evt->payload;
            ESP_LOGI(TAG, "N1 RFID_MGMT_ACK action=%u status=%u tarjetas=%u",
                     ack->action, ack->status, ack->count);
            for (uint8_t i = 0; i < ack->count && i < IOT_RFID_MAX_CARDS; i++) {
                char uid_buf[24];
                format_uid_hex(ack->cards[i].uid, ack->cards[i].uid_len, uid_buf, sizeof(uid_buf));
                ESP_LOGI(TAG, "  [%u] %s", (unsigned int)i + 1U, uid_buf);
            }
        }
        break;

    case OP_ALARM_EVENT:
        if (evt->len == sizeof(ble_alarm_event_t)) {
            const ble_alarm_event_t *pkt = (const ble_alarm_event_t *)evt->payload;
            if (!ble_dedup_check(&s_dedup, src_addr, OP_ALARM_EVENT, pkt->seq_num)) {
                return;
            }
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_global.alarm_state = (iot_alarm_state_t)pkt->new_state;
            s_global.alarm_fail_cnt = pkt->fail_counter;
            s_global.pir_active = (pkt->flags & 0x01U) != 0U;
            refresh_gateway_rgb_locked();
            xSemaphoreGive(s_state_mutex);

            ESP_LOGW(TAG, "N2 ALARMA=%s reason=%u fail=%u/3 lat_local=%" PRIu32 " us",
                     iot_alarm_state_str((iot_alarm_state_t)pkt->new_state),
                     pkt->reason, pkt->fail_counter, pkt->local_latency_us);
        }
        break;

    case OP_CLIMA_TELEMETRY:
        if (evt->len == sizeof(ble_clima_telemetry_t)) {
            const ble_clima_telemetry_t *pkt = (const ble_clima_telemetry_t *)evt->payload;
            if (!ble_dedup_check(&s_dedup, src_addr, OP_CLIMA_TELEMETRY, pkt->seq_num)) {
                return;
            }
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_global.temp_deci_c = pkt->temp_deci_c;
            s_global.delta_deci_c = pkt->delta_deci_c;
            s_global.humidity_pct = pkt->humidity_pct;
            s_global.temp_alert = (iot_temp_alert_t)pkt->alert_state;
            s_global.t_high_c = pkt->threshold_high_c;
            s_global.clima_valid = true;
            refresh_gateway_rgb_locked();
            xSemaphoreGive(s_state_mutex);

            ESP_LOGI(TAG, "N3 CLIMA T=%.1fC (dT=%+.1fC) H=%u%% alerta=%s T_high=%uC",
                     pkt->temp_deci_c / 10.0,
                     pkt->delta_deci_c / 10.0,
                     pkt->humidity_pct,
                     iot_temp_alert_str((iot_temp_alert_t)pkt->alert_state),
                     pkt->threshold_high_c);
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
                    }
                    refresh_gateway_rgb_locked();
                    xSemaphoreGive(s_state_mutex);

                    ESP_LOGI(TAG, "Nodo N%d (%s) -> %s (conectados=%u/3)",
                             idx + 1,
                             (idx == 0) ? BLE_ADV_NAME_NODO1 : ((idx == 1) ? BLE_ADV_NAME_NODO2 : BLE_ADV_NAME_NODO3),
                             evt.ready ? "ONLINE" : "DESCONECTADO",
                             ble_central_star_connected_count());

                    if (evt.ready) {
                        const ble_status_req_t req = { .req_flags = 0x01U };
                        (void)ble_central_star_send_cmd(evt.role, OP_STATUS_REQ, &req, sizeof(req));
                    }
                }
            }
        }

        const int64_t now = esp_timer_get_time();
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        for (int i = 0; i < 3; i++) {
            if (s_global.links[i].connected &&
                !s_global.links[i].offline_warned &&
                (now - s_global.links[i].last_seen_us) > S3_OFFLINE_TIMEOUT_US) {
                s_global.links[i].offline_warned = true;
                ESP_LOGW(TAG, "Timeout heartbeat > 45s en Nodo N%d", i + 1);
            }
        }
        xSemaphoreGive(s_state_mutex);

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
    const ble_status_req_t req = { .req_flags = req_flags };
    const iot_node_role_t roles[] = { NODE_ROLE_ACCESO, NODE_ROLE_SEGURIDAD, NODE_ROLE_CLIMA };
    for (size_t i = 0; i < 3U; i++) {
        (void)ble_central_star_send_cmd(roles[i], OP_STATUS_REQ, &req, sizeof(req));
    }
}

void star_log_global_status(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const star_global_state_t st = s_global;
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG,
             "ESTADO GLOBAL | Cerradura=%s | Alarma=%s (fallos=%u/3) | Clima=%s T=%.1fC (dT=%+.1fC) H=%u%% alerta=%s T_high=%uC | BLE=%u/3",
             iot_lock_state_str(st.lock_state),
             iot_alarm_state_str(st.alarm_state),
             st.alarm_fail_cnt,
             st.clima_valid ? "OK" : "SIN_DATO",
             st.temp_deci_c / 10.0,
             st.delta_deci_c / 10.0,
             st.humidity_pct,
             iot_temp_alert_str(st.temp_alert),
             st.t_high_c,
             ble_central_star_connected_count());
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
