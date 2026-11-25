#include "ble_central_star.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#include "ble_protocol.h"
#include "sdkconfig.h"

void ble_store_config_init(void);

static const char *TAG = "ble_central_star";

#define STAR_NUM_SLOTS 3

typedef enum {
    SLOT_DISCONNECTED = 0,
    SLOT_CONNECTING,
    SLOT_DISCOVERING,
    SLOT_READY,
} slot_state_t;

typedef struct {
    iot_node_role_t role;
    const char *adv_name;
    slot_state_t state;
    uint16_t conn_handle;
    uint16_t svc_start_handle;
    uint16_t svc_end_handle;
    uint16_t evt_tx_val_handle;
    uint16_t cmd_rx_def_handle;
    uint16_t cmd_rx_val_handle;
    uint16_t evt_tx_cccd_handle;
    int64_t connected_ts_us;
} star_slot_t;

static star_slot_t s_slots[STAR_NUM_SLOTS] = {
    { .role = NODE_ROLE_ACCESO,    .adv_name = BLE_ADV_NAME_NODO1, .conn_handle = BLE_HS_CONN_HANDLE_NONE },
    { .role = NODE_ROLE_SEGURIDAD, .adv_name = BLE_ADV_NAME_NODO2, .conn_handle = BLE_HS_CONN_HANDLE_NONE },
    { .role = NODE_ROLE_CLIMA,     .adv_name = BLE_ADV_NAME_NODO3, .conn_handle = BLE_HS_CONN_HANDLE_NONE },
};

static const ble_uuid16_t s_svc_uuid = BLE_UUID16_INIT(BLE_GATT_SVC_UUID);

static ble_central_cfg_t s_cfg;
static portMUX_TYPE s_slots_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static int s_connecting_slot = -1;
static bool s_synced = false;
static bool s_initialized = false;

static int ble_central_gap_event(struct ble_gap_event *event, void *arg);
static void ble_central_maybe_scan(void);

static int role_to_idx(iot_node_role_t role)
{
    if (role < NODE_ROLE_ACCESO || role > NODE_ROLE_CLIMA) {
        return -1;
    }
    return (int)role - 1;
}

static int find_slot_by_conn_locked(uint16_t conn_handle)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return -1;
    }
    for (int i = 0; i < STAR_NUM_SLOTS; i++) {
        if (s_slots[i].state != SLOT_DISCONNECTED && s_slots[i].conn_handle == conn_handle) {
            return i;
        }
    }
    return -1;
}

static void reset_slot_locked(int idx)
{
    s_slots[idx].state = SLOT_DISCONNECTED;
    s_slots[idx].conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_slots[idx].svc_start_handle = 0;
    s_slots[idx].svc_end_handle = 0;
    s_slots[idx].evt_tx_val_handle = 0;
    s_slots[idx].cmd_rx_def_handle = 0;
    s_slots[idx].cmd_rx_val_handle = 0;
    s_slots[idx].evt_tx_cccd_handle = 0;
    s_slots[idx].connected_ts_us = 0;
}

static void abort_discovery_and_disconnect(uint16_t conn_handle, const char *step)
{
    ESP_LOGW(TAG, "Fallo descubrimiento GATT (%s) en conn_handle=%u, desconectando", step, conn_handle);
    (void)ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

static int on_cccd_write_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr,
                            void *arg)
{
    (void)attr;
    (void)arg;

    if (error->status != 0) {
        abort_discovery_and_disconnect(conn_handle, "write CCCD");
        return 0;
    }

    iot_node_role_t ready_role = NODE_ROLE_GATEWAY;
    const char *ready_name = NULL;

    portENTER_CRITICAL(&s_slots_lock);
    const int idx = find_slot_by_conn_locked(conn_handle);
    if (idx >= 0) {
        s_slots[idx].state = SLOT_READY;
        ready_role = s_slots[idx].role;
        ready_name = s_slots[idx].adv_name;
    }
    portEXIT_CRITICAL(&s_slots_lock);

    if (ready_name) {
        ESP_LOGI(TAG, "Periferico %s LISTO (conn_handle=%u, Notify 0xFF11 activo)",
                 ready_name, conn_handle);
        if (s_cfg.on_node_state) {
            s_cfg.on_node_state(ready_role, true, s_cfg.user_ctx);
        }
    }

    ble_central_maybe_scan();
    return 0;
}

static int on_disc_dsc_cb(uint16_t conn_handle,
                          const struct ble_gatt_error *error,
                          uint16_t chr_val_handle,
                          const struct ble_gatt_dsc *dsc,
                          void *arg)
{
    (void)chr_val_handle;
    (void)arg;

    if (error->status == 0 && dsc != NULL) {
        if (ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
            portENTER_CRITICAL(&s_slots_lock);
            const int idx = find_slot_by_conn_locked(conn_handle);
            if (idx >= 0 && s_slots[idx].evt_tx_cccd_handle == 0) {
                s_slots[idx].evt_tx_cccd_handle = dsc->handle;
            }
            portEXIT_CRITICAL(&s_slots_lock);
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        uint16_t cccd_handle = 0;
        portENTER_CRITICAL(&s_slots_lock);
        const int idx = find_slot_by_conn_locked(conn_handle);
        if (idx >= 0) {
            cccd_handle = s_slots[idx].evt_tx_cccd_handle;
        }
        portEXIT_CRITICAL(&s_slots_lock);

        if (cccd_handle == 0) {
            abort_discovery_and_disconnect(conn_handle, "CCCD no encontrado");
            return 0;
        }

        const uint8_t cccd_val[2] = { 0x01U, 0x00U };
        const int rc = ble_gattc_write_flat(conn_handle, cccd_handle,
                                            cccd_val, sizeof(cccd_val),
                                            on_cccd_write_cb, NULL);
        if (rc != 0) {
            abort_discovery_and_disconnect(conn_handle, "ble_gattc_write_flat CCCD");
        }
        return 0;
    }

    abort_discovery_and_disconnect(conn_handle, "disc_all_dscs");
    return 0;
}

static int on_disc_chr_cb(uint16_t conn_handle,
                          const struct ble_gatt_error *error,
                          const struct ble_gatt_chr *chr,
                          void *arg)
{
    (void)arg;

    if (error->status == 0 && chr != NULL) {
        const uint16_t u16 = ble_uuid_u16(&chr->uuid.u);
        portENTER_CRITICAL(&s_slots_lock);
        const int idx = find_slot_by_conn_locked(conn_handle);
        if (idx >= 0) {
            if (u16 == BLE_GATT_CHR_EVT_TX_UUID) {
                s_slots[idx].evt_tx_val_handle = chr->val_handle;
            } else if (u16 == BLE_GATT_CHR_CMD_RX_UUID) {
                s_slots[idx].cmd_rx_def_handle = chr->def_handle;
                s_slots[idx].cmd_rx_val_handle = chr->val_handle;
            }
        }
        portEXIT_CRITICAL(&s_slots_lock);
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        uint16_t evt_h = 0;
        uint16_t cmd_h = 0;
        uint16_t end_h = 0;
        portENTER_CRITICAL(&s_slots_lock);
        const int idx = find_slot_by_conn_locked(conn_handle);
        if (idx >= 0) {
            evt_h = s_slots[idx].evt_tx_val_handle;
            cmd_h = s_slots[idx].cmd_rx_val_handle;
            end_h = s_slots[idx].svc_end_handle;
        }
        portEXIT_CRITICAL(&s_slots_lock);

        if (evt_h == 0 || cmd_h == 0 || evt_h >= end_h) {
            abort_discovery_and_disconnect(conn_handle, "0xFF11/0xFF12 no encontradas");
            return 0;
        }

        const int rc = ble_gattc_disc_all_dscs(conn_handle, evt_h, end_h, on_disc_dsc_cb, NULL);
        if (rc != 0) {
            abort_discovery_and_disconnect(conn_handle, "ble_gattc_disc_all_dscs");
        }
        return 0;
    }

    abort_discovery_and_disconnect(conn_handle, "disc_all_chrs");
    return 0;
}

static int on_disc_svc_cb(uint16_t conn_handle,
                          const struct ble_gatt_error *error,
                          const struct ble_gatt_svc *service,
                          void *arg)
{
    (void)arg;

    if (error->status == 0 && service != NULL) {
        portENTER_CRITICAL(&s_slots_lock);
        const int idx = find_slot_by_conn_locked(conn_handle);
        if (idx >= 0) {
            s_slots[idx].svc_start_handle = service->start_handle;
            s_slots[idx].svc_end_handle = service->end_handle;
        }
        portEXIT_CRITICAL(&s_slots_lock);
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        uint16_t start_h = 0;
        uint16_t end_h = 0;
        portENTER_CRITICAL(&s_slots_lock);
        const int idx = find_slot_by_conn_locked(conn_handle);
        if (idx >= 0) {
            start_h = s_slots[idx].svc_start_handle;
            end_h = s_slots[idx].svc_end_handle;
        }
        portEXIT_CRITICAL(&s_slots_lock);

        if (start_h == 0 || end_h == 0) {
            abort_discovery_and_disconnect(conn_handle, "servicio 0xFF10 ausente");
            return 0;
        }

        const int rc = ble_gattc_disc_all_chrs(conn_handle, start_h, end_h, on_disc_chr_cb, NULL);
        if (rc != 0) {
            abort_discovery_and_disconnect(conn_handle, "ble_gattc_disc_all_chrs");
        }
        return 0;
    }

    abort_discovery_and_disconnect(conn_handle, "disc_svc_by_uuid");
    return 0;
}

static int on_mtu_cb(uint16_t conn_handle,
                     const struct ble_gatt_error *error,
                     uint16_t mtu,
                     void *arg)
{
    (void)arg;
    if (error->status == 0) {
        ESP_LOGI(TAG, "MTU acordado conn_handle=%u mtu=%u", conn_handle, mtu);
    }

    const int rc = ble_gattc_disc_svc_by_uuid(conn_handle, &s_svc_uuid.u, on_disc_svc_cb, NULL);
    if (rc != 0) {
        abort_discovery_and_disconnect(conn_handle, "ble_gattc_disc_svc_by_uuid");
    }
    return 0;
}

static int on_cmd_write_cb(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr,
                           void *arg)
{
    (void)attr;
    (void)arg;
    if (error->status != 0) {
        ESP_LOGW(TAG, "Write CMD_RX con error status=%d conn_handle=%u", error->status, conn_handle);
    }
    return 0;
}

static void ble_central_maybe_scan(void)
{
    bool need_scan = false;

    portENTER_CRITICAL(&s_slots_lock);
    if (s_synced && s_connecting_slot < 0) {
        for (int i = 0; i < STAR_NUM_SLOTS; i++) {
            if (s_slots[i].state == SLOT_DISCONNECTED) {
                need_scan = true;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_slots_lock);

    if (!need_scan || ble_gap_disc_active()) {
        return;
    }

    struct ble_gap_disc_params disc_params;
    memset(&disc_params, 0, sizeof(disc_params));
    disc_params.filter_duplicates = 1;
    disc_params.passive = 0;
    disc_params.itvl = 0;
    disc_params.window = 0;

    const int rc = ble_gap_disc(s_own_addr_type, 10000, &disc_params, ble_central_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY && rc != BLE_HS_EBUSY) {
        ESP_LOGW(TAG, "Error iniciando escaneo BLE (rc=%d)", rc);
    }
}

static void handle_adv_report(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) {
        return;
    }
    if (!fields.name || fields.name_len == 0U) {
        return;
    }

    int target_idx = -1;
    portENTER_CRITICAL(&s_slots_lock);
    if (s_connecting_slot < 0) {
        for (int i = 0; i < STAR_NUM_SLOTS; i++) {
            const size_t nlen = strlen(s_slots[i].adv_name);
            if (s_slots[i].state == SLOT_DISCONNECTED &&
                fields.name_len == nlen &&
                memcmp(fields.name, s_slots[i].adv_name, nlen) == 0) {
                s_slots[i].state = SLOT_CONNECTING;
                s_connecting_slot = i;
                target_idx = i;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_slots_lock);

    if (target_idx < 0) {
        return;
    }

    ESP_LOGI(TAG, "Detectado %s (RSSI=%d dBm), conectando...",
             s_slots[target_idx].adv_name, disc->rssi);

    if (ble_gap_disc_active()) {
        (void)ble_gap_disc_cancel();
    }

    const int rc = ble_gap_connect(s_own_addr_type, &disc->addr, 10000, NULL,
                                   ble_central_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "Fallo ble_gap_connect para %s (rc=%d)",
                 s_slots[target_idx].adv_name, rc);
        portENTER_CRITICAL(&s_slots_lock);
        reset_slot_locked(target_idx);
        s_connecting_slot = -1;
        portEXIT_CRITICAL(&s_slots_lock);
        ble_central_maybe_scan();
    }
}

static int ble_central_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        handle_adv_report(&event->disc);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ble_central_maybe_scan();
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            const uint16_t ch = event->connect.conn_handle;
            const char *name = "?";
            portENTER_CRITICAL(&s_slots_lock);
            const int idx = s_connecting_slot;
            if (idx >= 0 && idx < STAR_NUM_SLOTS) {
                s_slots[idx].state = SLOT_DISCOVERING;
                s_slots[idx].conn_handle = ch;
                s_slots[idx].connected_ts_us = esp_timer_get_time();
                name = s_slots[idx].adv_name;
            }
            s_connecting_slot = -1;
            portEXIT_CRITICAL(&s_slots_lock);

            ESP_LOGI(TAG, "Enlace fisico establecido con %s (conn_handle=%u)", name, ch);
            const int rc = ble_gattc_exchange_mtu(ch, on_mtu_cb, NULL);
            if (rc != 0) {
                abort_discovery_and_disconnect(ch, "exchange_mtu");
            }
        } else {
            ESP_LOGW(TAG, "Fallo conexion pendiente (status=%d)", event->connect.status);
            portENTER_CRITICAL(&s_slots_lock);
            if (s_connecting_slot >= 0 && s_connecting_slot < STAR_NUM_SLOTS) {
                reset_slot_locked(s_connecting_slot);
            }
            s_connecting_slot = -1;
            portEXIT_CRITICAL(&s_slots_lock);
            ble_central_maybe_scan();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        iot_node_role_t role = NODE_ROLE_GATEWAY;
        const char *name = "?";
        bool was_ready = false;

        portENTER_CRITICAL(&s_slots_lock);
        const int idx = find_slot_by_conn_locked(event->disconnect.conn.conn_handle);
        if (idx >= 0) {
            role = s_slots[idx].role;
            name = s_slots[idx].adv_name;
            was_ready = (s_slots[idx].state == SLOT_READY);
            reset_slot_locked(idx);
            if (s_connecting_slot == idx) {
                s_connecting_slot = -1;
            }
        }
        portEXIT_CRITICAL(&s_slots_lock);

        ESP_LOGW(TAG, "Desconectado %s (reason=0x%02X)", name, event->disconnect.reason);
        if (was_ready && s_cfg.on_node_state) {
            s_cfg.on_node_state(role, false, s_cfg.user_ctx);
        }
        ble_central_maybe_scan();
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
        const int64_t rx_ts_us = esp_timer_get_time();
        iot_node_role_t role = NODE_ROLE_GATEWAY;
        uint16_t evt_h = 0;

        portENTER_CRITICAL(&s_slots_lock);
        const int idx = find_slot_by_conn_locked(event->notify_rx.conn_handle);
        if (idx >= 0) {
            role = s_slots[idx].role;
            evt_h = s_slots[idx].evt_tx_val_handle;
        }
        portEXIT_CRITICAL(&s_slots_lock);

        if (idx < 0 || event->notify_rx.attr_handle != evt_h) {
            return 0;
        }

        const uint16_t pkt_len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (pkt_len < 1U || pkt_len > BLE_PROTO_MAX_FRAME_LEN) {
            return 0;
        }

        uint8_t buf[BLE_PROTO_MAX_FRAME_LEN];
        uint16_t copied = 0;
        if (ble_hs_mbuf_to_flat(event->notify_rx.om, buf, sizeof(buf), &copied) == 0 && copied >= 1U) {
            if (s_cfg.on_rx) {
                s_cfg.on_rx(role, buf[0], &buf[1], (size_t)(copied - 1U), rx_ts_us, s_cfg.user_ctx);
            }
        }
        return 0;
    }

    default:
        return 0;
    }
}

static void ble_central_on_reset(int reason)
{
    ESP_LOGW(TAG, "Host NimBLE Central reset (reason=%d)", reason);
}

static void ble_central_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error asegurando direccion BLE S3 (rc=%d)", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error determinando tipo direccion BLE S3 (rc=%d)", rc);
        return;
    }

    uint8_t addr[6] = { 0 };
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "Maestro S3 MAC=%02X:%02X:%02X:%02X:%02X:%02X (iniciando escaneo estrella)",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    portENTER_CRITICAL(&s_slots_lock);
    s_synced = true;
    portEXIT_CRITICAL(&s_slots_lock);

    ble_central_maybe_scan();
}

static void ble_central_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "Tarea Host NimBLE Central ejecutandose en Core %d", xPortGetCoreID());
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_central_star_init(const ble_central_cfg_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        return ESP_OK;
    }

    s_cfg = *cfg;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init fallo: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = ble_central_on_reset;
    ble_hs_cfg.sync_cb = ble_central_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_att_set_preferred_mtu(128);
#ifdef CONFIG_BT_NIMBLE_GAP_SERVICE
    ble_svc_gap_init();
    (void)ble_svc_gap_device_name_set(BLE_ADV_NAME_GATEWAY);
#endif
    ble_store_config_init();

    nimble_port_freertos_init(ble_central_host_task);
    s_initialized = true;
    return ESP_OK;
}

esp_err_t ble_central_star_send_cmd(iot_node_role_t role,
                                    uint8_t opcode,
                                    const void *payload,
                                    size_t len)
{
    const int idx = role_to_idx(role);
    if (idx < 0 || (!payload && len > 0U) || (1U + len) > BLE_PROTO_MAX_FRAME_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
    uint16_t cmd_handle = 0;

    portENTER_CRITICAL(&s_slots_lock);
    if (s_slots[idx].state == SLOT_READY) {
        conn_handle = s_slots[idx].conn_handle;
        cmd_handle = s_slots[idx].cmd_rx_val_handle;
    }
    portEXIT_CRITICAL(&s_slots_lock);

    if (conn_handle == BLE_HS_CONN_HANDLE_NONE || cmd_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t frame[BLE_PROTO_MAX_FRAME_LEN];
    frame[0] = opcode;
    if (len > 0U) {
        memcpy(&frame[1], payload, len);
    }

    const int rc = ble_gattc_write_flat(conn_handle, cmd_handle, frame, (uint16_t)(1U + len),
                                        on_cmd_write_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "Error escribiendo opcode=0x%02X hacia %s (rc=%d)",
                 opcode, s_slots[idx].adv_name, rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool ble_central_star_get_info(iot_node_role_t role, ble_central_node_info_t *out_info)
{
    const int idx = role_to_idx(role);
    if (idx < 0 || !out_info) {
        return false;
    }

    memset(out_info, 0, sizeof(*out_info));
    out_info->role = role;
    out_info->name = s_slots[idx].adv_name;
    out_info->conn_handle = BLE_HS_CONN_HANDLE_NONE;

    int64_t ts = 0;
    portENTER_CRITICAL(&s_slots_lock);
    out_info->ready = (s_slots[idx].state == SLOT_READY);
    out_info->connecting = (s_slots[idx].state == SLOT_CONNECTING || s_slots[idx].state == SLOT_DISCOVERING);
    out_info->conn_handle = s_slots[idx].conn_handle;
    ts = s_slots[idx].connected_ts_us;
    portEXIT_CRITICAL(&s_slots_lock);

    if (out_info->ready && out_info->conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        int8_t rssi = 0;
        if (ble_gap_conn_rssi(out_info->conn_handle, &rssi) == 0) {
            out_info->rssi = rssi;
        }
        if (ts > 0) {
            out_info->uptime_s = (uint32_t)((esp_timer_get_time() - ts) / 1000000LL);
        }
    }
    return true;
}

uint8_t ble_central_star_connected_count(void)
{
    uint8_t cnt = 0;
    portENTER_CRITICAL(&s_slots_lock);
    for (int i = 0; i < STAR_NUM_SLOTS; i++) {
        if (s_slots[i].state == SLOT_READY) {
            cnt++;
        }
    }
    portEXIT_CRITICAL(&s_slots_lock);
    return cnt;
}
