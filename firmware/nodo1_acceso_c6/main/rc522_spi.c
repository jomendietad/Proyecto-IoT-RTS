#include "rc522_spi.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rc522_spi";

/* Pinout exacto según docs/hardware/pinout.md (Nodo 1 ESP32-C6-DevKitC-1) */
#define RC522_PIN_MISO GPIO_NUM_2
#define RC522_PIN_SCK  GPIO_NUM_6
#define RC522_PIN_MOSI GPIO_NUM_7
#define RC522_PIN_CS   GPIO_NUM_18
#define RC522_PIN_RST  GPIO_NUM_19

/* Registros MFRC522 */
#define REG_COMMAND      0x01U
#define REG_COM_IRQ      0x04U
#define REG_ERROR        0x06U
#define REG_FIFO_DATA    0x09U
#define REG_FIFO_LEVEL   0x0AU
#define REG_CONTROL      0x0CU
#define REG_BIT_FRAMING  0x0DU
#define REG_MODE         0x11U
#define REG_TX_CONTROL   0x14U
#define REG_TX_ASK       0x15U
#define REG_T_MODE       0x2AU
#define REG_T_PRESCALER  0x2BU
#define REG_T_RELOAD_H   0x2CU
#define REG_T_RELOAD_L   0x2DU
#define REG_VERSION      0x37U

/* Comandos MFRC522 / PICC */
#define PCD_IDLE         0x00U
#define PCD_TRANSCEIVE   0x0CU
#define PCD_RESETPHASE   0x0FU
#define PICC_REQIDL      0x26U
#define PICC_ANTICOLL_1  0x93U
#define PICC_HALT        0x50U

static spi_device_handle_t s_spi = NULL;
static bool s_present = false;

static esp_err_t rc522_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)((reg << 1) & 0x7EU), val };
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
    };
    return spi_device_transmit(s_spi, &t);
}

static uint8_t rc522_read_reg(uint8_t reg)
{
    uint8_t tx[2] = { (uint8_t)(((reg << 1) & 0x7EU) | 0x80U), 0x00U };
    uint8_t rx[2] = { 0, 0 };
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    if (spi_device_transmit(s_spi, &t) != ESP_OK) {
        return 0;
    }
    return rx[1];
}

static void rc522_set_bit_mask(uint8_t reg, uint8_t mask)
{
    rc522_write_reg(reg, (uint8_t)(rc522_read_reg(reg) | mask));
}

static void rc522_clear_bit_mask(uint8_t reg, uint8_t mask)
{
    rc522_write_reg(reg, (uint8_t)(rc522_read_reg(reg) & (uint8_t)(~mask)));
}

static esp_err_t rc522_to_card(uint8_t cmd,
                               const uint8_t *send_data,
                               uint8_t send_len,
                               uint8_t *back_data,
                               uint8_t *back_bits)
{
    rc522_write_reg(REG_COMMAND, PCD_IDLE);
    rc522_write_reg(REG_COM_IRQ, 0x7FU);
    rc522_set_bit_mask(REG_FIFO_LEVEL, 0x80U);

    for (uint8_t i = 0; i < send_len; i++) {
        rc522_write_reg(REG_FIFO_DATA, send_data[i]);
    }

    rc522_write_reg(REG_COMMAND, cmd);
    if (cmd == PCD_TRANSCEIVE) {
        rc522_set_bit_mask(REG_BIT_FRAMING, 0x80U);
    }

    uint8_t irq = 0;
    for (int i = 0; i < 30; i++) {
        irq = rc522_read_reg(REG_COM_IRQ);
        if (irq & 0x30U) {
            break;
        }
        if (irq & 0x01U) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    rc522_clear_bit_mask(REG_BIT_FRAMING, 0x80U);
    if (!(irq & 0x30U)) {
        return ESP_ERR_TIMEOUT;
    }
    if (rc522_read_reg(REG_ERROR) & 0x1BU) {
        return ESP_FAIL;
    }

    if (back_data && back_bits) {
        uint8_t n = rc522_read_reg(REG_FIFO_LEVEL);
        const uint8_t last_bits = (uint8_t)(rc522_read_reg(REG_CONTROL) & 0x07U);
        *back_bits = last_bits ? (uint8_t)((n - 1U) * 8U + last_bits) : (uint8_t)(n * 8U);
        if (n > 16U) {
            n = 16U;
        }
        for (uint8_t i = 0; i < n; i++) {
            back_data[i] = rc522_read_reg(REG_FIFO_DATA);
        }
    }
    return ESP_OK;
}

esp_err_t rc522_spi_init(void)
{
    const gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << RC522_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_cfg));
    gpio_set_level(RC522_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(RC522_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    const spi_bus_config_t bus_cfg = {
        .miso_io_num = RC522_PIN_MISO,
        .mosi_io_num = RC522_PIN_MOSI,
        .sclk_io_num = RC522_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }

    const spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 4 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = RC522_PIN_CS,
        .queue_size = 4,
    };
    err = spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi);
    if (err != ESP_OK) {
        return err;
    }

    rc522_write_reg(REG_COMMAND, PCD_RESETPHASE);
    vTaskDelay(pdMS_TO_TICKS(5));

    const uint8_t ver = rc522_read_reg(REG_VERSION);
    if (ver == 0x00U || ver == 0xFFU) {
        s_present = false;
        ESP_LOGW(TAG, "RC522 no detectado en SPI (VersionReg=0x%02X); usar comando 'sim_ok'/'sim_fail' en consola", ver);
        return ESP_ERR_NOT_FOUND;
    }

    rc522_write_reg(REG_T_MODE, 0x8DU);
    rc522_write_reg(REG_T_PRESCALER, 0x3EU);
    rc522_write_reg(REG_T_RELOAD_L, 30U);
    rc522_write_reg(REG_T_RELOAD_H, 0U);
    rc522_write_reg(REG_TX_ASK, 0x40U);
    rc522_write_reg(REG_MODE, 0x3DU);

    if (!(rc522_read_reg(REG_TX_CONTROL) & 0x03U)) {
        rc522_set_bit_mask(REG_TX_CONTROL, 0x03U);
    }

    s_present = true;
    ESP_LOGI(TAG, "RC522 inicializado (VersionReg=0x%02X)", ver);
    return ESP_OK;
}

bool rc522_spi_is_present(void)
{
    return s_present;
}

esp_err_t rc522_read_uid(uint8_t *uid_out, uint8_t *uid_len_out)
{
    if (!s_present || !uid_out || !uid_len_out) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t back[16] = {0};
    uint8_t back_bits = 0;

    rc522_write_reg(REG_BIT_FRAMING, 0x07U);
    uint8_t req = PICC_REQIDL;
    if (rc522_to_card(PCD_TRANSCEIVE, &req, 1U, back, &back_bits) != ESP_OK || back_bits != 0x10U) {
        return ESP_ERR_NOT_FOUND;
    }

    rc522_write_reg(REG_BIT_FRAMING, 0x00U);
    uint8_t anticoll[2] = { PICC_ANTICOLL_1, 0x20U };
    if (rc522_to_card(PCD_TRANSCEIVE, anticoll, 2U, back, &back_bits) != ESP_OK || back_bits < 40U) {
        return ESP_FAIL;
    }

    const uint8_t bcc = (uint8_t)(back[0] ^ back[1] ^ back[2] ^ back[3]);
    if (bcc != back[4]) {
        return ESP_ERR_INVALID_CRC;
    }

    memcpy(uid_out, back, 4U);
    *uid_len_out = 4U;

    uint8_t halt[2] = { PICC_HALT, 0x00U };
    rc522_to_card(PCD_TRANSCEIVE, halt, 2U, NULL, NULL);
    return ESP_OK;
}
