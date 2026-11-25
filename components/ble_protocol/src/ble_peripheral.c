#include "ble_peripheral.h"

#include <string.h>
#include "esp_log.h"
#include "sdkconfig.h"

#if defined(CONFIG_BT_NIMBLE_ENABLED) && defined(CONFIG_BT_NIMBLE_ROLE_PERIPHERAL)

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_protocol.h"

void ble_store_config_init(void);

static const char *TAG = "ble_peripheral";

static ble_peripheral_cfg_t s_cfg;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_evt_tx_val_handle = 0;
static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static bool s_initialized = false;
static bool s_connected = false;
static bool s_subscribed = false;

static uint8_t s_last_evt_buf[BLE_PROTO_MAX_FRAME_LEN] = { 0 };
static size_t s_last_evt_len = 1;

static const ble_uuid16_t s_svc_uuid = BLE_UUID16_INIT(BLE_GATT_SVC_UUID);
static const ble_uuid16_t s_chr_evt_uuid = BLE_UUID16_INIT(BLE_GATT_CHR_EVT_TX_UUID);
static const ble_uuid16_t s_chr_cmd_uuid = BLE_UUID16_INIT(BLE_GATT_CHR_CMD_RX_UUID);

static int ble_prph_gap_event(struct ble_gap_event *event, void *arg);
static void ble_prph_start_advertising(void);

static int ble_prph_gatt_access(uint16_t conn_handle,
                                uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt,
                                void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && attr_handle == s_evt_tx_val_handle) {
        uint8_t copy[BLE_PROTO_MAX_FRAME_LEN];
        size_t copy_len = 0;

        portENTER_CRITICAL(&s_state_lock);
        copy_len = s_last_evt_len;
        if (copy_len > sizeof(copy)) {
            copy_len = sizeof(copy);
        }
        memcpy(copy, s_last_evt_buf, copy_len);
        portEXIT_CRITICAL(&s_state_lock);

        const int rc = os_mbuf_append(ctxt->om, copy, (uint16_t)copy_len);
        return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR &&
        ble_uuid_cmp(ctxt->chr->uuid, &s_chr_cmd_uuid.u) == 0) {
        const uint16_t pkt_len = OS_MBUF_PKTLEN(ctxt->om);
        if (pkt_len < 1U || pkt_len > BLE_PROTO_MAX_FRAME_LEN) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        uint8_t rx_buf[BLE_PROTO_MAX_FRAME_LEN];
        uint16_t copied = 0;
        const int rc = ble_hs_mbuf_to_flat(ctxt->om, rx_buf, sizeof(rx_buf), &copied);
        if (rc != 0 || copied < 1U) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        if (s_cfg.on_cmd) {
            s_cfg.on_cmd(rx_buf[0], &rx_buf[1], (size_t)(copied - 1U), s_cfg.user_ctx);
        }
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_chr_evt_uuid.u,
                .access_cb = ble_prph_gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_evt_tx_val_handle,
            },
            {
                .uuid = &s_chr_cmd_uuid.u,
                .access_cb = ble_prph_gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            { 0 },
        },
    },
    { 0 },
};

static void ble_prph_start_advertising(void)
{
    if (ble_gap_adv_active()) {
        return;
    }

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)(uintptr_t)name;
    fields.name_len = (uint8_t)strlen(name);
    fields.name_is_complete = 1;

    static const ble_uuid16_t adv_uuids16[] = {
        BLE_UUID16_INIT(BLE_GATT_SVC_UUID),
    };
    fields.uuids16 = adv_uuids16;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error configurando campos ADV (rc=%d)", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_prph_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "Error iniciando Advertising (rc=%d)", rc);
    } else {
        ESP_LOGI(TAG, "Advertising activo como '%s' (svc 0x%04X)", name, BLE_GATT_SVC_UUID);
    }
}

static int ble_prph_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            portENTER_CRITICAL(&s_state_lock);
            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            s_subscribed = false;
            portEXIT_CRITICAL(&s_state_lock);

            ESP_LOGI(TAG, "Conectado a Maestro S3 (conn_handle=%u)", event->connect.conn_handle);
            if (s_cfg.on_conn) {
                s_cfg.on_conn(true, false, s_cfg.user_ctx);
            }
        } else {
            ESP_LOGW(TAG, "Fallo conexion GAP (status=%d), reiniciando ADV", event->connect.status);
            ble_prph_start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_evt_tx_val_handle) {
            const bool sub = (event->subscribe.cur_notify != 0);
            portENTER_CRITICAL(&s_state_lock);
            s_subscribed = sub;
            portEXIT_CRITICAL(&s_state_lock);

            ESP_LOGI(TAG, "Suscripcion Notify 0xFF11: %s (conn_handle=%u)",
                     sub ? "HABILITADA" : "DESHABILITADA", event->subscribe.conn_handle);
            if (s_cfg.on_conn) {
                s_cfg.on_conn(true, sub, s_cfg.user_ctx);
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "Desconectado del Maestro (reason=0x%02X), reanudando ADV",
                 event->disconnect.reason);
        portENTER_CRITICAL(&s_state_lock);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_connected = false;
        s_subscribed = false;
        portEXIT_CRITICAL(&s_state_lock);

        if (s_cfg.on_conn) {
            s_cfg.on_conn(false, false, s_cfg.user_ctx);
        }
        ble_prph_start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (!ble_peripheral_is_connected()) {
            ble_prph_start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU negociado: %u bytes (conn_handle=%u)",
                 event->mtu.value, event->mtu.conn_handle);
        return 0;

    default:
        return 0;
    }
}

static void ble_prph_on_reset(int reason)
{
    ESP_LOGW(TAG, "Host NimBLE reset (reason=%d)", reason);
}

static void ble_prph_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error asegurando direccion BLE (rc=%d)", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error determinando tipo de direccion BLE (rc=%d)", rc);
        return;
    }

    uint8_t addr[6] = { 0 };
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "BLE MAC=%02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    ble_prph_start_advertising();
}

static void ble_prph_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "Tarea Host NimBLE iniciada");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_peripheral_init(const ble_peripheral_cfg_t *cfg)
{
    if (!cfg || !cfg->device_name) {
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

    ble_hs_cfg.reset_cb = ble_prph_on_reset;
    ble_hs_cfg.sync_cb = ble_prph_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_att_set_preferred_mtu(128);
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(s_cfg.device_name);
    if (rc != 0) {
        return ESP_FAIL;
    }

    ble_store_config_init();
    nimble_port_freertos_init(ble_prph_host_task);
    s_initialized = true;
    return ESP_OK;
}

esp_err_t ble_peripheral_notify(uint8_t opcode, const void *payload, size_t len)
{
    if ((!payload && len > 0U) || (1U + len) > BLE_PROTO_MAX_FRAME_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t frame[BLE_PROTO_MAX_FRAME_LEN];
    frame[0] = opcode;
    if (len > 0U) {
        memcpy(&frame[1], payload, len);
    }

    uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
    bool can_notify = false;

    portENTER_CRITICAL(&s_state_lock);
    memcpy(s_last_evt_buf, frame, 1U + len);
    s_last_evt_len = 1U + len;
    conn = s_conn_handle;
    can_notify = (s_connected && s_subscribed && conn != BLE_HS_CONN_HANDLE_NONE);
    portEXIT_CRITICAL(&s_state_lock);

    if (!can_notify) {
        return ESP_ERR_INVALID_STATE;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, (uint16_t)(1U + len));
    if (!om) {
        return ESP_ERR_NO_MEM;
    }

    const int rc = ble_gatts_notify_custom(conn, s_evt_tx_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "Error en Notify opcode=0x%02X rc=%d", opcode, rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool ble_peripheral_is_connected(void)
{
    bool ret;
    portENTER_CRITICAL(&s_state_lock);
    ret = s_connected;
    portEXIT_CRITICAL(&s_state_lock);
    return ret;
}

bool ble_peripheral_is_subscribed(void)
{
    bool ret;
    portENTER_CRITICAL(&s_state_lock);
    ret = s_connected && s_subscribed;
    portEXIT_CRITICAL(&s_state_lock);
    return ret;
}

#else

esp_err_t ble_peripheral_init(const ble_peripheral_cfg_t *cfg)
{
    (void)cfg;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_peripheral_notify(uint8_t opcode, const void *payload, size_t len)
{
    (void)opcode;
    (void)payload;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_peripheral_is_connected(void)
{
    return false;
}

bool ble_peripheral_is_subscribed(void)
{
    return false;
}

#endif
