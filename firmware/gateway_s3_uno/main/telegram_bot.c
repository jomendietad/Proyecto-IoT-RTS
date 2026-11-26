#include "telegram_bot.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "ble_central_star.h"
#include "ble_protocol.h"
#include "iot_common_types.h"
#include "star_coordinator.h"
#include "status_led.h"
#include "wifi_manager.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

static const char *TAG = "tg_bot";

#define TG_API_BASE_URL       "https://api.telegram.org/bot"
#define TG_TX_URGENT_Q_LEN    8
#define TG_TX_NORMAL_Q_LEN    12
#define TG_HTTP_RX_BUF_SZ     16384U
#define TG_HTTP_TX_BUF_SZ     2560U
#define TG_ALERT_DEADLINE_US  3000000U /* D_urgent = 3000 ms segun diseno_rtos_str.md */

typedef enum {
    TG_TX_TEXT = 0,
    TG_TX_PANEL,
    TG_TX_CB_ACK,
} tg_tx_kind_t;

typedef struct {
    tg_tx_kind_t kind;
    bool urgent;
    bool enqueued_wifi_online;
    int64_t enqueued_us;
    char *text;
    char cb_id[68];
} tg_tx_item_t;

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} tg_http_rx_buf_t;

static str_metrics_ctx_t *s_metrics = NULL;
static tg_get_s3_tasks_cb_t s_tasks_cb = NULL;
static QueueHandle_t s_tx_urgent_q = NULL;
static QueueHandle_t s_tx_normal_q = NULL;
static SemaphoreHandle_t s_tx_wake_sem = NULL;
static TaskHandle_t s_tx_task = NULL;
static TaskHandle_t s_rx_task = NULL;

static volatile bool s_wifi_online = false;
static bool s_creds_valid = false;
static bool s_boot_notified = false;
static int64_t s_next_update_id = 0;

static const char *const s_panel_markup_json =
    "{\"inline_keyboard\":["
    "[{\"text\":\"🛡️ Armar Alarma\",\"callback_data\":\"cmd:arm\"},"
    "{\"text\":\"🔓 Desarmar Alarma\",\"callback_data\":\"cmd:disarm\"}],"
    "[{\"text\":\"🔑 Abrir Cerradura\",\"callback_data\":\"cmd:lock_open\"},"
    "{\"text\":\"🔒 Cerrar Cerradura\",\"callback_data\":\"cmd:lock_close\"}],"
    "[{\"text\":\"📊 Estado Global\",\"callback_data\":\"cmd:status\"},"
    "{\"text\":\"📡 Estado Red BLE\",\"callback_data\":\"cmd:net\"}],"
    "[{\"text\":\"⏱️ Métricas Tiempo Real (STR)\",\"callback_data\":\"cmd:metrics\"}]"
    "]}";

static void *tg_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(size);
    }
    return p;
}

static char *tg_strdup(const char *s)
{
    if (!s) {
        return NULL;
    }
    const size_t len = strlen(s) + 1U;
    char *dup = (char *)tg_alloc(len);
    if (dup) {
        memcpy(dup, s, len);
    }
    return dup;
}

static bool credentials_are_configured(void)
{
    if (TELEGRAM_BOT_TOKEN[0] == '\0' ||
        strstr(TELEGRAM_BOT_TOKEN, "REEMPLAZA") != NULL) {
        return false;
    }
    if ((int64_t)TELEGRAM_AUTHORIZED_CHAT_ID == 0LL) {
        return false;
    }
    return true;
}

/* ============================================================================
 * Utilidades JSON autocontenidas en C puro (constructor y parser descendente)
 * ============================================================================ */

static size_t json_append_raw(char *dst, size_t cap, size_t pos, const char *src)
{
    if (!dst || !src || pos >= cap) {
        return pos;
    }
    while (*src != '\0' && pos + 1U < cap) {
        dst[pos++] = *src++;
    }
    dst[pos] = '\0';
    return pos;
}

static size_t json_append_escaped(char *dst, size_t cap, size_t pos, const char *src)
{
    if (!dst || !src || pos >= cap) {
        return pos;
    }
    while (*src != '\0' && pos + 6U < cap) {
        const unsigned char c = (unsigned char)*src++;
        switch (c) {
        case '"':  dst[pos++] = '\\'; dst[pos++] = '"'; break;
        case '\\': dst[pos++] = '\\'; dst[pos++] = '\\'; break;
        case '\n': dst[pos++] = '\\'; dst[pos++] = 'n'; break;
        case '\r': dst[pos++] = '\\'; dst[pos++] = 'r'; break;
        case '\t': dst[pos++] = '\\'; dst[pos++] = 't'; break;
        default:
            if (c < 0x20U) {
                pos += (size_t)snprintf(dst + pos, cap - pos, "\\u%04x", (unsigned int)c);
            } else {
                dst[pos++] = (char)c;
            }
            break;
        }
    }
    dst[pos] = '\0';
    return pos;
}

static const char *json_skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    return p;
}

static const char *json_skip_string(const char *p, const char *end)
{
    if (p >= end || *p != '"') {
        return NULL;
    }
    p++;
    while (p < end) {
        if (*p == '\\') {
            p += 2;
            continue;
        }
        if (*p == '"') {
            return p + 1;
        }
        p++;
    }
    return NULL;
}

static const char *json_skip_value(const char *p, const char *end)
{
    p = json_skip_ws(p, end);
    if (p >= end) {
        return NULL;
    }

    if (*p == '"') {
        return json_skip_string(p, end);
    }

    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (p < end) {
            if (*p == '"') {
                p = json_skip_string(p, end);
                if (!p) {
                    return NULL;
                }
                continue;
            }
            if (*p == '{' || *p == '[') {
                depth++;
            } else if (*p == '}' || *p == ']') {
                depth--;
                if (depth == 0) {
                    return p + 1;
                }
            }
            p++;
        }
        return NULL;
    }

    while (p < end && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        p++;
    }
    return p;
}

static bool json_key_equals(const char *str_start, const char *str_end, const char *key)
{
    if (!str_start || !str_end || str_end - str_start < 2 || *str_start != '"') {
        return false;
    }
    const size_t raw_len = (size_t)((str_end - str_start) - 2);
    return (strlen(key) == raw_len && strncmp(str_start + 1, key, raw_len) == 0);
}

static bool json_find_key_in_obj(const char *obj_start,
                                 const char *obj_end,
                                 const char *key,
                                 const char **out_val_start,
                                 const char **out_val_end)
{
    const char *p = json_skip_ws(obj_start, obj_end);
    if (!p || p >= obj_end || *p != '{') {
        return false;
    }
    p++;

    while (p < obj_end) {
        p = json_skip_ws(p, obj_end);
        if (p >= obj_end || *p == '}') {
            return false;
        }
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '"') {
            return false;
        }

        const char *k_start = p;
        const char *k_end = json_skip_string(p, obj_end);
        if (!k_end) {
            return false;
        }

        p = json_skip_ws(k_end, obj_end);
        if (p >= obj_end || *p != ':') {
            return false;
        }
        p = json_skip_ws(p + 1, obj_end);
        const char *v_start = p;
        const char *v_end = json_skip_value(v_start, obj_end);
        if (!v_end) {
            return false;
        }

        if (json_key_equals(k_start, k_end, key)) {
            if (out_val_start) {
                *out_val_start = v_start;
            }
            if (out_val_end) {
                *out_val_end = v_end;
            }
            return true;
        }
        p = v_end;
    }
    return false;
}

static bool json_parse_int64(const char *val_start, const char *val_end, int64_t *out)
{
    if (!val_start || !val_end || val_start >= val_end || !out) {
        return false;
    }
    char buf[32];
    const size_t n = (size_t)(val_end - val_start);
    if (n >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, val_start, n);
    buf[n] = '\0';

    char *endptr = NULL;
    const long long v = strtoll(buf, &endptr, 10);
    if (endptr == buf) {
        return false;
    }
    *out = (int64_t)v;
    return true;
}

static bool json_parse_bool(const char *val_start, const char *val_end, bool *out)
{
    if (!val_start || !val_end || !out) {
        return false;
    }
    const size_t n = (size_t)(val_end - val_start);
    if (n == 4U && strncmp(val_start, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (n == 5U && strncmp(val_start, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool json_extract_string(const char *val_start, const char *val_end, char *out, size_t out_sz)
{
    if (!val_start || !val_end || !out || out_sz == 0U ||
        (val_end - val_start) < 2 || *val_start != '"') {
        return false;
    }

    const char *p = val_start + 1;
    const char *limit = val_end - 1;
    size_t pos = 0;

    while (p < limit && pos + 1U < out_sz) {
        if (*p == '\\' && (p + 1) < limit) {
            p++;
            switch (*p) {
            case '"':  out[pos++] = '"'; p++; break;
            case '\\': out[pos++] = '\\'; p++; break;
            case '/':  out[pos++] = '/'; p++; break;
            case 'n':  out[pos++] = '\n'; p++; break;
            case 'r':  out[pos++] = '\r'; p++; break;
            case 't':  out[pos++] = '\t'; p++; break;
            case 'u':
                if (p + 4 < limit) {
                    char hex[5] = { p[1], p[2], p[3], p[4], '\0' };
                    const unsigned long code = strtoul(hex, NULL, 16);
                    if (code >= 0x20UL && code <= 0x7EUL) {
                        out[pos++] = (char)code;
                    }
                    p += 5;
                } else {
                    p++;
                }
                break;
            default:
                out[pos++] = *p++;
                break;
            }
        } else {
            out[pos++] = *p++;
        }
    }
    out[pos] = '\0';
    return true;
}

/* ============================================================================
 * Cliente HTTP persistente (keep-alive) hacia api.telegram.org
 * ============================================================================ */

static esp_err_t tg_http_event_handler(esp_http_client_event_t *evt)
{
    tg_http_rx_buf_t *rx = (tg_http_rx_buf_t *)evt->user_data;
    if (!rx || !rx->buf) {
        return ESP_OK;
    }

    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        const size_t avail = (rx->cap > rx->len + 1U) ? (rx->cap - rx->len - 1U) : 0U;
        const size_t copy_len = ((size_t)evt->data_len <= avail) ? (size_t)evt->data_len : avail;
        if (copy_len > 0U) {
            memcpy(rx->buf + rx->len, evt->data, copy_len);
            rx->len += copy_len;
            rx->buf[rx->len] = '\0';
        }
        if ((size_t)evt->data_len > avail) {
            rx->overflow = true;
        }
    }
    return ESP_OK;
}

static void tg_http_close(esp_http_client_handle_t *client)
{
    if (client && *client) {
        esp_http_client_cleanup(*client);
        *client = NULL;
    }
}

static esp_err_t tg_http_post_json(esp_http_client_handle_t *client,
                                   const char *method,
                                   const char *json_body,
                                   int timeout_ms,
                                   tg_http_rx_buf_t *rx_buf,
                                   int *out_status_code)
{
    char url[192];
    snprintf(url, sizeof(url), "%s%s/%s", TG_API_BASE_URL, TELEGRAM_BOT_TOKEN, method);

    rx_buf->len = 0;
    rx_buf->overflow = false;
    rx_buf->buf[0] = '\0';

    if (*client == NULL) {
        const esp_http_client_config_t cfg = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = timeout_ms,
            .keep_alive_enable = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .event_handler = tg_http_event_handler,
            .user_data = rx_buf,
            .buffer_size = 2048,
            .buffer_size_tx = 2048,
        };
        *client = esp_http_client_init(&cfg);
        if (!*client) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        esp_http_client_set_url(*client, url);
        esp_http_client_set_method(*client, HTTP_METHOD_POST);
        esp_http_client_set_timeout_ms(*client, timeout_ms);
    }

    esp_http_client_set_header(*client, "Content-Type", "application/json");
    esp_http_client_set_post_field(*client, json_body, (int)strlen(json_body));

    const esp_err_t err = esp_http_client_perform(*client);
    if (err != ESP_OK) {
        tg_http_close(client);
        return err;
    }

    const int status = esp_http_client_get_status_code(*client);
    if (out_status_code) {
        *out_status_code = status;
    }
    return (status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}

/* ============================================================================
 * Despacho de comandos y callbacks del Bot
 * ============================================================================ */

static esp_err_t tg_enqueue_cb_ack(const char *cb_id)
{
    if (!s_tx_normal_q || !s_tx_wake_sem || !cb_id || cb_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    tg_tx_item_t item = {
        .kind = TG_TX_CB_ACK,
        .urgent = false,
        .enqueued_us = esp_timer_get_time(),
    };
    strlcpy(item.cb_id, cb_id, sizeof(item.cb_id));
    if (xQueueSend(s_tx_normal_q, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(s_tx_wake_sem);
    return ESP_OK;
}

static void handle_cmd_status(void)
{
    star_cmd_request_status_all(0x01U);
    vTaskDelay(pdMS_TO_TICKS(250));

    star_global_snapshot_t st;
    star_coordinator_get_snapshot(&st);
    wifi_status_t wst;
    wifi_manager_get_status(&wst);

    char clima_line[96];
    if (st.clima_valid) {
        snprintf(clima_line, sizeof(clima_line),
                 "%.1f °C (dT %+.1f °C) | H=%u%% | %s (T_high=%u °C)",
                 st.temp_deci_c / 10.0,
                 st.delta_deci_c / 10.0,
                 st.humidity_pct,
                 iot_temp_alert_str(st.temp_alert),
                 st.t_high_c);
    } else {
        snprintf(clima_line, sizeof(clima_line), "Sin lectura aun (T_high=%u °C)", st.t_high_c);
    }

    char msg[512];
    snprintf(msg, sizeof(msg),
             "📊 ESTADO GLOBAL DEL SISTEMA\n"
             "• Alarma: %s (fallos RFID: %u/3, PIR=%s)\n"
             "• Cerradura: %s\n"
             "• Clima (N3): %s\n"
             "• Red BLE: %u/3 (N1=%s, N2=%s, N3=%s)\n"
             "• Wi-Fi: %s (%s, RSSI=%d dBm, IP=%s)",
             iot_alarm_state_str(st.alarm_state),
             st.alarm_fail_cnt,
             st.pir_active ? "ACTIVO" : "reposo",
             iot_lock_state_str(st.lock_state),
             clima_line,
             ble_central_star_connected_count(),
             st.nodes[0].connected ? "OK" : "OFF",
             st.nodes[1].connected ? "OK" : "OFF",
             st.nodes[2].connected ? "OK" : "OFF",
             wst.connected ? "ONLINE" : "OFFLINE",
             wst.ssid[0] ? wst.ssid : "-",
             wst.rssi,
             wst.ip[0] ? wst.ip : "-");
    (void)telegram_bot_send(TG_PRIO_NORMAL, msg);
}

static void handle_cmd_net(void)
{
    wifi_status_t wst;
    wifi_manager_get_status(&wst);
    star_global_snapshot_t st;
    star_coordinator_get_snapshot(&st);

    const iot_node_role_t roles[] = { NODE_ROLE_ACCESO, NODE_ROLE_SEGURIDAD, NODE_ROLE_CLIMA };
    char msg[512];
    int pos = snprintf(msg, sizeof(msg),
                       "📡 RED BLE GATT EN ESTRELLA (%u/3)\n",
                       ble_central_star_connected_count());

    for (size_t i = 0; i < 3U && pos > 0 && (size_t)pos < sizeof(msg) - 96U; i++) {
        ble_central_node_info_t info;
        if (ble_central_star_get_info(roles[i], &info)) {
            const uint16_t heap_kb = st.nodes[i].has_hb ? st.nodes[i].hb.free_heap_kb : 0U;
            pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                            "• N%u (%s): %s | RSSI=%d dBm | up=%" PRIu32 "s | heap=%u KB\n",
                            info.role,
                            info.name,
                            info.ready ? "READY" : (info.connecting ? "CONNECTING" : "OFFLINE"),
                            info.rssi,
                            info.uptime_s,
                            (unsigned int)heap_kb);
        }
    }

    snprintf(msg + pos, sizeof(msg) - (size_t)pos,
             "📶 Wi-Fi STA: %s\n• SSID: %s (slot %u)\n• IP: %s | RSSI: %d dBm | Failovers: %" PRIu32,
             wst.connected ? "ONLINE" : "OFFLINE",
             wst.ssid[0] ? wst.ssid : "-",
             (unsigned int)wst.active_slot,
             wst.ip[0] ? wst.ip : "-",
             wst.rssi,
             wst.failovers);
    (void)telegram_bot_send(TG_PRIO_NORMAL, msg);
}

static void handle_cmd_metrics(void)
{
    star_cmd_request_status_all(0x02U);
    vTaskDelay(pdMS_TO_TICKS(300));

    uint32_t s3_max_lat = 0;
    uint32_t s3_avg_jit = 0;
    uint16_t s3_misses = 0;
    str_metrics_snapshot(s_metrics, &s3_max_lat, &s3_avg_jit, &s3_misses);

    uint16_t s3_min_stack = 0xFFFFU;
    if (s_tasks_cb) {
        const TaskHandle_t *handles = NULL;
        const char *const *names = NULL;
        size_t count = 0;
        s_tasks_cb(&handles, &names, &count);
        (void)names;
        for (size_t i = 0; i < count; i++) {
            if (handles && handles[i] != NULL) {
                const uint16_t bytes = (uint16_t)(uxTaskGetStackHighWaterMark(handles[i]) * sizeof(StackType_t));
                if (bytes < s3_min_stack) {
                    s3_min_stack = bytes;
                }
            }
        }
    }
    if (s3_min_stack == 0xFFFFU) {
        s3_min_stack = 0U;
    }

    const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t min_int = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t total_psram = esp_psram_get_size();

    star_global_snapshot_t st;
    star_coordinator_get_snapshot(&st);

    char msg[768];
    int pos = snprintf(msg, sizeof(msg),
                       "⏱️ METRICAS EN TIEMPO REAL (STR)\n"
                       "• Gateway S3: max_lat=%" PRIu32 " us | avg_jit=%" PRIu32 " us | misses=%u | min_stack=%u B\n"
                       "• SRAM interna: %u KB libres (min=%u KB)\n"
                       "• Octal PSRAM: %u KB libres / %u KB total\n",
                       s3_max_lat, s3_avg_jit, s3_misses, (unsigned int)s3_min_stack,
                       (unsigned int)(free_int / 1024U), (unsigned int)(min_int / 1024U),
                       (unsigned int)(free_psram / 1024U), (unsigned int)(total_psram / 1024U));

    for (int i = 0; i < 3 && pos > 0 && (size_t)pos < sizeof(msg) - 96U; i++) {
        if (st.nodes[i].has_metrics) {
            const ble_metrics_report_t *m = &st.nodes[i].metrics;
            pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                            "• N%d: max_lat=%" PRIu32 " us | avg_jit=%" PRIu32 " us | misses=%u | min_stack=%u B\n",
                            i + 1,
                            m->max_crit_latency_us,
                            m->avg_jitter_us,
                            m->deadline_misses,
                            (unsigned int)m->min_stack_free_bytes);
        } else {
            pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                            "• N%d: sin reporte reciente (%s)\n",
                            i + 1, st.nodes[i].connected ? "online" : "offline");
        }
    }
    (void)telegram_bot_send(TG_PRIO_NORMAL, msg);
}

static void handle_cmd_alarm(bool arm)
{
    const iot_alarm_state_t target = arm ? ALARM_STATE_ARMADA : ALARM_STATE_DESARMADA;
    const esp_err_t err = star_cmd_alarm(target);
    if (err != ESP_OK) {
        (void)telegram_bot_send(TG_PRIO_NORMAL,
                                "❌ No se pudo enviar comando a Nodo 2 (desconectado de la estrella BLE).");
    }
}

static void handle_cmd_lock(bool open)
{
    const iot_lock_state_t target = open ? LOCK_STATE_ABIERTA : LOCK_STATE_CERRADA;
    const esp_err_t err = star_cmd_lock(target);
    if (err != ESP_OK) {
        (void)telegram_bot_send(TG_PRIO_NORMAL,
                                "❌ No se pudo enviar comando a Nodo 1 (desconectado de la estrella BLE).");
    }
}

static void handle_cmd_threshold(const char *arg)
{
    if (!arg || arg[0] == '\0') {
        (void)telegram_bot_send(TG_PRIO_NORMAL, "Uso: /umbral <15..50> (ej. /umbral 28)");
        return;
    }
    char *endptr = NULL;
    const long val = strtol(arg, &endptr, 10);
    if (endptr == arg || val < IOT_TEMP_THRESH_MIN_C || val > IOT_TEMP_THRESH_MAX_C) {
        (void)telegram_bot_send(TG_PRIO_NORMAL,
                                "❌ Umbral invalido. Debe estar entre 15 y 50 °C (ej. /umbral 28).");
        return;
    }
    const esp_err_t err = star_cmd_threshold((uint8_t)val);
    if (err != ESP_OK) {
        (void)telegram_bot_send(TG_PRIO_NORMAL,
                                "❌ No se pudo actualizar umbral: Nodo 3 desconectado de la estrella BLE.");
    } else {
        char msg[96];
        snprintf(msg, sizeof(msg), "🌡️ Umbral superior enviado a Nodo 3: %ld °C", val);
        (void)telegram_bot_send(TG_PRIO_NORMAL, msg);
    }
}

static void handle_cmd_rfid(const char *arg)
{
    if (!arg || arg[0] == '\0') {
        (void)telegram_bot_send(TG_PRIO_NORMAL,
                                "Uso: /rfid list | /rfid add <UID> | /rfid del <UID>");
        return;
    }

    if (strcmp(arg, "list") == 0) {
        if (star_cmd_rfid(BLE_RFID_ACT_LIST, NULL, 0U) != ESP_OK) {
            (void)telegram_bot_send(TG_PRIO_NORMAL, "❌ Nodo 1 desconectado de la estrella BLE.");
        }
        return;
    }

    ble_rfid_action_t act = BLE_RFID_ACT_LIST;
    const char *uid_str = NULL;
    if (strncmp(arg, "add ", 4) == 0) {
        act = BLE_RFID_ACT_ADD;
        uid_str = arg + 4;
    } else if (strncmp(arg, "del ", 4) == 0) {
        act = BLE_RFID_ACT_DEL;
        uid_str = arg + 4;
    } else {
        (void)telegram_bot_send(TG_PRIO_NORMAL,
                                "Uso: /rfid list | /rfid add <UID> | /rfid del <UID>");
        return;
    }

    while (*uid_str == ' ') {
        uid_str++;
    }

    uint8_t uid[IOT_RFID_UID_MAX_LEN];
    uint8_t len = 0;
    if (!star_parse_uid_hex(uid_str, uid, &len)) {
        (void)telegram_bot_send(TG_PRIO_NORMAL,
                                "❌ UID invalido. Usa 4 o 7 bytes hex (ej. /rfid add A1B2C3D4).");
        return;
    }

    if (star_cmd_rfid(act, uid, len) != ESP_OK) {
        (void)telegram_bot_send(TG_PRIO_NORMAL, "❌ Nodo 1 desconectado de la estrella BLE.");
    }
}

static void dispatch_callback_action(const char *data)
{
    (void)status_led_flash_activity(80);
    if (strcmp(data, "cmd:arm") == 0) {
        handle_cmd_alarm(true);
    } else if (strcmp(data, "cmd:disarm") == 0) {
        handle_cmd_alarm(false);
    } else if (strcmp(data, "cmd:lock_open") == 0) {
        handle_cmd_lock(true);
    } else if (strcmp(data, "cmd:lock_close") == 0) {
        handle_cmd_lock(false);
    } else if (strcmp(data, "cmd:status") == 0) {
        handle_cmd_status();
    } else if (strcmp(data, "cmd:net") == 0) {
        handle_cmd_net();
    } else if (strcmp(data, "cmd:metrics") == 0) {
        handle_cmd_metrics();
    } else {
        ESP_LOGW(TAG, "Callback desconocido: %s", data);
    }
}

static void dispatch_text_command(const char *raw_text)
{
    if (!raw_text) {
        return;
    }

    while (*raw_text == ' ' || *raw_text == '\t') {
        raw_text++;
    }
    if (*raw_text != '/') {
        return;
    }

    char cmd[32] = { 0 };
    size_t i = 0;
    const char *p = raw_text;
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '@' && i + 1U < sizeof(cmd)) {
        cmd[i++] = (char)tolower((unsigned char)*p++);
    }
    cmd[i] = '\0';

    /* Saltar posible sufijo @NombreDelBot en grupos */
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        p++;
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    char arg[96] = { 0 };
    strlcpy(arg, p, sizeof(arg));
    size_t arg_len = strlen(arg);
    while (arg_len > 0U && (arg[arg_len - 1U] == ' ' || arg[arg_len - 1U] == '\r' || arg[arg_len - 1U] == '\n')) {
        arg[--arg_len] = '\0';
    }

    ESP_LOGI(TAG, "Comando Telegram recibido: %s (arg='%s')", cmd, arg);
    (void)status_led_flash_activity(80);

    if (strcmp(cmd, "/panel") == 0 || strcmp(cmd, "/start") == 0 || strcmp(cmd, "/help") == 0) {
        (void)telegram_bot_send_panel();
    } else if (strcmp(cmd, "/estado") == 0) {
        handle_cmd_status();
    } else if (strcmp(cmd, "/armar") == 0) {
        handle_cmd_alarm(true);
    } else if (strcmp(cmd, "/desarmar") == 0) {
        handle_cmd_alarm(false);
    } else if (strcmp(cmd, "/cerradura") == 0) {
        if (strcasecmp(arg, "abrir") == 0) {
            handle_cmd_lock(true);
        } else if (strcasecmp(arg, "cerrar") == 0) {
            handle_cmd_lock(false);
        } else {
            (void)telegram_bot_send(TG_PRIO_NORMAL, "Uso: /cerradura abrir | /cerradura cerrar");
        }
    } else if (strcmp(cmd, "/umbral") == 0) {
        handle_cmd_threshold(arg);
    } else if (strcmp(cmd, "/rfid") == 0) {
        handle_cmd_rfid(arg);
    } else if (strcmp(cmd, "/red") == 0) {
        handle_cmd_net();
    } else if (strcmp(cmd, "/metricas") == 0) {
        handle_cmd_metrics();
    } else {
        (void)telegram_bot_send(TG_PRIO_NORMAL,
                                "Comando no reconocido. Usa /panel, /estado, /armar, /desarmar, "
                                "/cerradura abrir|cerrar, /umbral <C>, /rfid list|add|del, /red o /metricas.");
    }
}

static void process_single_update(const char *obj_start, const char *obj_end)
{
    const char *v_start = NULL;
    const char *v_end = NULL;
    int64_t update_id = 0;

    if (json_find_key_in_obj(obj_start, obj_end, "update_id", &v_start, &v_end) &&
        json_parse_int64(v_start, v_end, &update_id)) {
        if (update_id >= s_next_update_id) {
            s_next_update_id = update_id + 1;
        }
    }

    const int64_t auth_chat_id = (int64_t)TELEGRAM_AUTHORIZED_CHAT_ID;

    if (json_find_key_in_obj(obj_start, obj_end, "message", &v_start, &v_end)) {
        const char *chat_s = NULL, *chat_e = NULL;
        const char *id_s = NULL, *id_e = NULL;
        int64_t chat_id = 0;

        if (json_find_key_in_obj(v_start, v_end, "chat", &chat_s, &chat_e) &&
            json_find_key_in_obj(chat_s, chat_e, "id", &id_s, &id_e) &&
            json_parse_int64(id_s, id_e, &chat_id)) {
            if (chat_id != auth_chat_id) {
                ESP_LOGW(TAG, "Mensaje rechazado de chat_id no autorizado: %" PRId64, chat_id);
                return;
            }
            const char *txt_s = NULL, *txt_e = NULL;
            if (json_find_key_in_obj(v_start, v_end, "text", &txt_s, &txt_e)) {
                char text[160];
                if (json_extract_string(txt_s, txt_e, text, sizeof(text))) {
                    dispatch_text_command(text);
                }
            }
        }
        return;
    }

    if (json_find_key_in_obj(obj_start, obj_end, "callback_query", &v_start, &v_end)) {
        const char *msg_s = NULL, *msg_e = NULL;
        const char *chat_s = NULL, *chat_e = NULL;
        const char *id_s = NULL, *id_e = NULL;
        int64_t chat_id = 0;

        if (json_find_key_in_obj(v_start, v_end, "message", &msg_s, &msg_e) &&
            json_find_key_in_obj(msg_s, msg_e, "chat", &chat_s, &chat_e) &&
            json_find_key_in_obj(chat_s, chat_e, "id", &id_s, &id_e) &&
            json_parse_int64(id_s, id_e, &chat_id)) {
            if (chat_id != auth_chat_id) {
                ESP_LOGW(TAG, "Callback rechazado de chat_id no autorizado: %" PRId64, chat_id);
                return;
            }

            const char *cb_id_s = NULL, *cb_id_e = NULL;
            char cb_id[68] = { 0 };
            if (json_find_key_in_obj(v_start, v_end, "id", &cb_id_s, &cb_id_e)) {
                (void)json_extract_string(cb_id_s, cb_id_e, cb_id, sizeof(cb_id));
            }
            (void)tg_enqueue_cb_ack(cb_id);

            const char *data_s = NULL, *data_e = NULL;
            char data[48] = { 0 };
            if (json_find_key_in_obj(v_start, v_end, "data", &data_s, &data_e) &&
                json_extract_string(data_s, data_e, data, sizeof(data))) {
                dispatch_callback_action(data);
            }
        }
    }
}

static void process_get_updates_response(const char *json, size_t len)
{
    const char *end = json + len;
    const char *ok_s = NULL, *ok_e = NULL;
    bool ok = false;
    if (!json_find_key_in_obj(json, end, "ok", &ok_s, &ok_e) ||
        !json_parse_bool(ok_s, ok_e, &ok) || !ok) {
        ESP_LOGW(TAG, "Respuesta getUpdates no valida o ok=false");
        return;
    }

    const char *res_s = NULL, *res_e = NULL;
    if (!json_find_key_in_obj(json, end, "result", &res_s, &res_e)) {
        return;
    }

    const char *p = json_skip_ws(res_s, res_e);
    if (!p || p >= res_e || *p != '[') {
        return;
    }
    p++;

    while (p < res_e) {
        p = json_skip_ws(p, res_e);
        if (p >= res_e || *p == ']') {
            break;
        }
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '{') {
            break;
        }
        const char *obj_end = json_skip_value(p, res_e);
        if (!obj_end) {
            break;
        }
        process_single_update(p, obj_end);
        p = obj_end;
    }
}

/* ============================================================================
 * Tareas FreeRTOS en Core 1: task_tg_tx (prio 8) y task_tg_rx (prio 5)
 * ============================================================================ */

static void free_tx_item(tg_tx_item_t *item)
{
    if (item && item->text) {
        free(item->text);
        item->text = NULL;
    }
}

static void task_tg_tx(void *arg)
{
    (void)arg;
    char *tx_body = (char *)tg_alloc(TG_HTTP_TX_BUF_SZ);
    char *rx_mem = (char *)tg_alloc(2048U);
    if (!tx_body || !rx_mem) {
        ESP_LOGE(TAG, "Sin memoria para buffers TX de Telegram");
        vTaskDelete(NULL);
        return;
    }

    tg_http_rx_buf_t rx_buf = { .buf = rx_mem, .cap = 2048U };
    esp_http_client_handle_t client = NULL;

    while (1) {
        if (!s_creds_valid || !s_wifi_online) {
            tg_http_close(&client);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (xSemaphoreTake(s_tx_wake_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;
        }

        tg_tx_item_t item = { 0 };
        bool got = (xQueueReceive(s_tx_urgent_q, &item, 0) == pdTRUE);
        if (!got) {
            got = (xQueueReceive(s_tx_normal_q, &item, 0) == pdTRUE);
        }
        if (!got) {
            continue;
        }

        const char *method = "sendMessage";
        size_t pos = 0;

        if (item.kind == TG_TX_CB_ACK) {
            method = "answerCallbackQuery";
            pos = json_append_raw(tx_body, TG_HTTP_TX_BUF_SZ, pos, "{\"callback_query_id\":\"");
            pos = json_append_escaped(tx_body, TG_HTTP_TX_BUF_SZ, pos, item.cb_id);
            pos = json_append_raw(tx_body, TG_HTTP_TX_BUF_SZ, pos, "\"}");
        } else {
            char hdr[96];
            snprintf(hdr, sizeof(hdr), "{\"chat_id\":%" PRId64 ",\"text\":\"",
                     (int64_t)TELEGRAM_AUTHORIZED_CHAT_ID);
            pos = json_append_raw(tx_body, TG_HTTP_TX_BUF_SZ, pos, hdr);
            pos = json_append_escaped(tx_body, TG_HTTP_TX_BUF_SZ, pos, item.text ? item.text : "");
            pos = json_append_raw(tx_body, TG_HTTP_TX_BUF_SZ, pos, "\"");
            if (item.kind == TG_TX_PANEL) {
                pos = json_append_raw(tx_body, TG_HTTP_TX_BUF_SZ, pos, ",\"reply_markup\":");
                pos = json_append_raw(tx_body, TG_HTTP_TX_BUF_SZ, pos, s_panel_markup_json);
            }
            pos = json_append_raw(tx_body, TG_HTTP_TX_BUF_SZ, pos, "}");
        }

        int status_code = 0;
        esp_err_t err = tg_http_post_json(&client, method, tx_body, 10000, &rx_buf, &status_code);
        if (err != ESP_OK && s_wifi_online) {
            vTaskDelay(pdMS_TO_TICKS(50));
            err = tg_http_post_json(&client, method, tx_body, 10000, &rx_buf, &status_code);
        }

        if (err == ESP_OK) {
            if (item.urgent && item.enqueued_wifi_online && s_metrics) {
                const uint32_t lat_us = (uint32_t)(esp_timer_get_time() - item.enqueued_us);
                str_metrics_record(s_metrics, "tg_alert_e2e_us", lat_us, TG_ALERT_DEADLINE_US, false);
            }
        } else {
            ESP_LOGW(TAG, "Error enviando %s (err=%s, http=%d)",
                     method, esp_err_to_name(err), status_code);
        }

        free_tx_item(&item);
    }
}

static void task_tg_rx(void *arg)
{
    (void)arg;
    char *rx_mem = (char *)tg_alloc(TG_HTTP_RX_BUF_SZ);
    if (!rx_mem) {
        ESP_LOGE(TAG, "Sin memoria para buffer RX de Telegram");
        vTaskDelete(NULL);
        return;
    }

    tg_http_rx_buf_t rx_buf = { .buf = rx_mem, .cap = TG_HTTP_RX_BUF_SZ };
    esp_http_client_handle_t client = NULL;

    while (1) {
        if (!s_creds_valid || !s_wifi_online) {
            tg_http_close(&client);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        char req_json[160];
        snprintf(req_json, sizeof(req_json),
                 "{\"offset\":%" PRId64 ",\"timeout\":15,\"allowed_updates\":[\"message\",\"callback_query\"]}",
                 s_next_update_id);

        int status_code = 0;
        const esp_err_t err = tg_http_post_json(&client, "getUpdates", req_json, 20000, &rx_buf, &status_code);
        if (err == ESP_OK && rx_buf.len > 0U) {
            if (rx_buf.overflow) {
                ESP_LOGW(TAG, "Buffer RX Telegram truncado (%u B)", (unsigned int)rx_buf.cap);
            }
            process_get_updates_response(rx_buf.buf, rx_buf.len);
        } else if (s_wifi_online) {
            ESP_LOGW(TAG, "Fallo getUpdates (err=%s, http=%d), reintentando...",
                     esp_err_to_name(err), status_code);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}

/* ============================================================================
 * API Publica
 * ============================================================================ */

esp_err_t telegram_bot_init(str_metrics_ctx_t *metrics,
                            tg_get_s3_tasks_cb_t tasks_cb,
                            TaskHandle_t *out_tx_task,
                            TaskHandle_t *out_rx_task)
{
    s_metrics = metrics;
    s_tasks_cb = tasks_cb;

    s_tx_urgent_q = xQueueCreate(TG_TX_URGENT_Q_LEN, sizeof(tg_tx_item_t));
    s_tx_normal_q = xQueueCreate(TG_TX_NORMAL_Q_LEN, sizeof(tg_tx_item_t));
    s_tx_wake_sem = xSemaphoreCreateCounting(TG_TX_URGENT_Q_LEN + TG_TX_NORMAL_Q_LEN + 4, 0);
    if (!s_tx_urgent_q || !s_tx_normal_q || !s_tx_wake_sem) {
        return ESP_ERR_NO_MEM;
    }

    s_creds_valid = credentials_are_configured();
    if (!s_creds_valid) {
        ESP_LOGW(TAG, "TELEGRAM_BOT_TOKEN o TELEGRAM_AUTHORIZED_CHAT_ID sin configurar en secrets.h; Bot en espera");
    } else {
        ESP_LOGI(TAG, "Bot Telegram listo (chat_id=%" PRId64 ", PSRAM=%u KB)",
                 (int64_t)TELEGRAM_AUTHORIZED_CHAT_ID,
                 (unsigned int)(esp_psram_get_size() / 1024U));
    }

    if (xTaskCreatePinnedToCore(task_tg_tx, "task_tg_tx", 8192, NULL, 8, &s_tx_task, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(task_tg_rx, "task_tg_rx", 8192, NULL, 5, &s_rx_task, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    if (out_tx_task) {
        *out_tx_task = s_tx_task;
    }
    if (out_rx_task) {
        *out_rx_task = s_rx_task;
    }
    return ESP_OK;
}

void telegram_bot_set_wifi_online(bool online)
{
    s_wifi_online = online;
    if (online && s_creds_valid && !s_boot_notified) {
        s_boot_notified = true;
        wifi_status_t wst;
        wifi_manager_get_status(&wst);
        char msg[224];
        snprintf(msg, sizeof(msg),
                 "🚀 Gateway ESP32-S3 en linea\n• Wi-Fi: %s (%s)\n• Estrella BLE: %u/3 nodos listos\nEnvia /panel para abrir los controles.",
                 wst.ssid[0] ? wst.ssid : "-",
                 wst.ip[0] ? wst.ip : "-",
                 ble_central_star_connected_count());
        (void)telegram_bot_send(TG_PRIO_NORMAL, msg);
    }
}

esp_err_t telegram_bot_send(tg_prio_t prio, const char *text)
{
    if (!s_creds_valid || !s_tx_urgent_q || !s_tx_normal_q || !s_tx_wake_sem || !text) {
        return ESP_ERR_INVALID_STATE;
    }

    char *copy = tg_strdup(text);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }

    tg_tx_item_t item = {
        .kind = TG_TX_TEXT,
        .urgent = (prio == TG_PRIO_URGENT),
        .enqueued_wifi_online = s_wifi_online,
        .enqueued_us = esp_timer_get_time(),
        .text = copy,
    };

    QueueHandle_t q = item.urgent ? s_tx_urgent_q : s_tx_normal_q;
    if (xQueueSend(q, &item, 0) != pdTRUE) {
        free_tx_item(&item);
        return ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(s_tx_wake_sem);
    return ESP_OK;
}

esp_err_t telegram_bot_send_panel(void)
{
    if (!s_creds_valid || !s_tx_normal_q || !s_tx_wake_sem) {
        return ESP_ERR_INVALID_STATE;
    }

    star_global_snapshot_t st;
    star_coordinator_get_snapshot(&st);

    char summary[256];
    snprintf(summary, sizeof(summary),
             "🎛️ PANEL DE CONTROL IoT\n"
             "Alarma: %s | Cerradura: %s\n"
             "Clima: %.1f °C (%s) | BLE: %u/3",
             iot_alarm_state_str(st.alarm_state),
             iot_lock_state_str(st.lock_state),
             st.clima_valid ? (st.temp_deci_c / 10.0) : 0.0,
             st.clima_valid ? iot_temp_alert_str(st.temp_alert) : "SIN_DATO",
             ble_central_star_connected_count());

    char *copy = tg_strdup(summary);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }

    tg_tx_item_t item = {
        .kind = TG_TX_PANEL,
        .urgent = false,
        .enqueued_wifi_online = s_wifi_online,
        .enqueued_us = esp_timer_get_time(),
        .text = copy,
    };

    if (xQueueSend(s_tx_normal_q, &item, 0) != pdTRUE) {
        free_tx_item(&item);
        return ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(s_tx_wake_sem);
    return ESP_OK;
}
