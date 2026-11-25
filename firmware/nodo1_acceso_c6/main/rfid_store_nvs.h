#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "iot_common_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t len;
    uint8_t uid[IOT_RFID_UID_MAX_LEN];
} rfid_card_entry_t;

esp_err_t rfid_store_init(void);

bool rfid_store_is_authorized(const uint8_t *uid, uint8_t len);

esp_err_t rfid_store_add(const uint8_t *uid, uint8_t len);

esp_err_t rfid_store_del(const uint8_t *uid, uint8_t len);

esp_err_t rfid_store_get_all(rfid_card_entry_t *out_cards, size_t max_cards, size_t *out_count);

void rfid_store_format_uid(const uint8_t *uid, uint8_t len, char *out_str, size_t out_size);

bool rfid_store_parse_hex(const char *hex_str, uint8_t *out_uid, uint8_t *out_len);

#ifdef __cplusplus
}
#endif
