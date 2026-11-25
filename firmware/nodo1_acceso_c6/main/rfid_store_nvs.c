#include "rfid_store_nvs.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "rfid_store";

#define RFID_NVS_NAMESPACE "rfid_store"
#define RFID_NVS_KEY       "cards_v1"

typedef struct __attribute__((packed)) {
    uint8_t count;
    rfid_card_entry_t cards[IOT_RFID_MAX_CARDS];
} rfid_nvs_blob_t;

static SemaphoreHandle_t s_store_mutex = NULL;
static rfid_nvs_blob_t s_cache;

static esp_err_t save_blob_locked(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RFID_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, RFID_NVS_KEY, &s_cache, sizeof(s_cache));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static bool uid_match(const rfid_card_entry_t *a, const uint8_t *uid, uint8_t len)
{
    return (a->len == len) && (memcmp(a->uid, uid, len) == 0);
}

esp_err_t rfid_store_init(void)
{
    if (!s_store_mutex) {
        s_store_mutex = xSemaphoreCreateMutex();
        if (!s_store_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    memset(&s_cache, 0, sizeof(s_cache));

    nvs_handle_t h;
    esp_err_t err = nvs_open(RFID_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    size_t sz = sizeof(s_cache);
    err = nvs_get_blob(h, RFID_NVS_KEY, &s_cache, &sz);
    nvs_close(h);

    if (err == ESP_ERR_NVS_NOT_FOUND || sz != sizeof(s_cache) || s_cache.count > IOT_RFID_MAX_CARDS) {
        memset(&s_cache, 0, sizeof(s_cache));
        /* UID por defecto autorizado (tarjeta RFID principal) */
        s_cache.count = 1;
        s_cache.cards[0].len = 4;
        s_cache.cards[0].uid[0] = 0xB6;
        s_cache.cards[0].uid[1] = 0x7B;
        s_cache.cards[0].uid[2] = 0x0C;
        s_cache.cards[0].uid[3] = 0x03;
        err = save_blob_locked();
        ESP_LOGW(TAG, "NVS RFID vacio: cargado UID por defecto B6:7B:0C:03");
    } else if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS RFID cargado (%u tarjeta(s) autorizada(s))", s_cache.count);
    }
    return err;
}

bool rfid_store_is_authorized(const uint8_t *uid, uint8_t len)
{
    if (!uid || len == 0 || len > IOT_RFID_UID_MAX_LEN || !s_store_mutex) {
        return false;
    }
    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    bool found = false;
    for (uint8_t i = 0; i < s_cache.count; i++) {
        if (uid_match(&s_cache.cards[i], uid, len)) {
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_store_mutex);
    return found;
}

esp_err_t rfid_store_add(const uint8_t *uid, uint8_t len)
{
    if (!uid || len == 0 || len > IOT_RFID_UID_MAX_LEN || !s_store_mutex) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < s_cache.count; i++) {
        if (uid_match(&s_cache.cards[i], uid, len)) {
            xSemaphoreGive(s_store_mutex);
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (s_cache.count >= IOT_RFID_MAX_CARDS) {
        xSemaphoreGive(s_store_mutex);
        return ESP_ERR_NO_MEM;
    }

    rfid_card_entry_t *dst = &s_cache.cards[s_cache.count++];
    memset(dst, 0, sizeof(*dst));
    dst->len = len;
    memcpy(dst->uid, uid, len);
    esp_err_t err = save_blob_locked();
    xSemaphoreGive(s_store_mutex);
    return err;
}

esp_err_t rfid_store_del(const uint8_t *uid, uint8_t len)
{
    if (!uid || len == 0 || len > IOT_RFID_UID_MAX_LEN || !s_store_mutex) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    int idx = -1;
    for (uint8_t i = 0; i < s_cache.count; i++) {
        if (uid_match(&s_cache.cards[i], uid, len)) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0) {
        xSemaphoreGive(s_store_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    for (uint8_t i = (uint8_t)idx; i + 1U < s_cache.count; i++) {
        s_cache.cards[i] = s_cache.cards[i + 1U];
    }
    s_cache.count--;
    memset(&s_cache.cards[s_cache.count], 0, sizeof(rfid_card_entry_t));
    esp_err_t err = save_blob_locked();
    xSemaphoreGive(s_store_mutex);
    return err;
}

esp_err_t rfid_store_get_all(rfid_card_entry_t *out_cards, size_t max_cards, size_t *out_count)
{
    if (!s_store_mutex || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    size_t n = s_cache.count;
    if (n > max_cards) {
        n = max_cards;
    }
    if (out_cards && n > 0) {
        memcpy(out_cards, s_cache.cards, n * sizeof(rfid_card_entry_t));
    }
    *out_count = n;
    xSemaphoreGive(s_store_mutex);
    return ESP_OK;
}

void rfid_store_format_uid(const uint8_t *uid, uint8_t len, char *out_str, size_t out_size)
{
    if (!out_str || out_size == 0) {
        return;
    }
    size_t pos = 0;
    out_str[0] = '\0';
    for (uint8_t i = 0; i < len && pos + 4 < out_size; i++) {
        int written = snprintf(out_str + pos, out_size - pos, "%s%02X", (i > 0) ? ":" : "", uid[i]);
        if (written < 0) {
            break;
        }
        pos += (size_t)written;
    }
}

bool rfid_store_parse_hex(const char *hex_str, uint8_t *out_uid, uint8_t *out_len)
{
    if (!hex_str || !out_uid || !out_len) {
        return false;
    }
    char clean[IOT_RFID_UID_MAX_LEN * 2U + 1U];
    size_t k = 0;
    for (size_t i = 0; hex_str[i] != '\0'; i++) {
        unsigned char c = (unsigned char)hex_str[i];
        if (c == ':' || c == '-' || c == ' ') {
            continue;
        }
        if (!isxdigit(c) || k >= (IOT_RFID_UID_MAX_LEN * 2U)) {
            return false;
        }
        clean[k++] = (char)toupper(c);
    }
    if (k == 0 || (k % 2U) != 0U) {
        return false;
    }
    const uint8_t bytes = (uint8_t)(k / 2U);
    for (uint8_t i = 0; i < bytes; i++) {
        unsigned int v = 0;
        if (sscanf(&clean[i * 2U], "%2x", &v) != 1) {
            return false;
        }
        out_uid[i] = (uint8_t)v;
    }
    *out_len = bytes;
    return true;
}
