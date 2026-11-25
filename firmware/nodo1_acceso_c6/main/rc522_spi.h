#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa el bus SPI2 y el lector RFID-RC522 (GPIO 2/6/7/18/19).
 * @return ESP_OK si el MFRC522 responde; ESP_ERR_NOT_FOUND si no hay módulo físico.
 */
esp_err_t rc522_spi_init(void);

/**
 * @brief Indica si el módulo RC522 fue detectado al iniciar.
 */
bool rc522_spi_is_present(void);

/**
 * @brief Intenta detectar una tarjeta MIFARE y leer su UID (4 bytes).
 */
esp_err_t rc522_read_uid(uint8_t *uid_out, uint8_t *uid_len_out);

#ifdef __cplusplus
}
#endif
