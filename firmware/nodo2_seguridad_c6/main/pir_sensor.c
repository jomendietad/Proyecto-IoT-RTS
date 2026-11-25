#include "pir_sensor.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "pir_sensor";

/* Pinout exacto según docs/hardware/pinout.md (Nodo 2 ESP32-C6-DevKitC-1) */
#define PIR_GPIO            GPIO_NUM_3
#define PIR_MIN_PERIOD_US   (500000LL)  /* T_min = 500 ms */
#define PIR_WARMUP_US       (5000000LL) /* Estabilización inicial 5 s en banco */

static TaskHandle_t s_alarm_task = NULL;
static uint32_t s_notify_bit = 0;
static volatile int64_t *s_isr_ts_out = NULL;
static volatile int64_t s_boot_us = 0;
static volatile int64_t s_last_isr_us = 0;

static void IRAM_ATTR pir_gpio_isr(void *arg)
{
    (void)arg;
    const int64_t now_us = esp_timer_get_time();

    if ((now_us - s_boot_us) < PIR_WARMUP_US) {
        return;
    }
    if ((now_us - s_last_isr_us) < PIR_MIN_PERIOD_US) {
        return;
    }
    s_last_isr_us = now_us;

    if (s_isr_ts_out) {
        *s_isr_ts_out = now_us;
    }

    if (s_alarm_task) {
        BaseType_t hpw = pdFALSE;
        xTaskNotifyFromISR(s_alarm_task, s_notify_bit, eSetBits, &hpw);
        if (hpw == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

esp_err_t pir_sensor_init(TaskHandle_t alarm_task, uint32_t notify_bit, volatile int64_t *isr_ts_out)
{
    s_alarm_task = alarm_task;
    s_notify_bit = notify_bit;
    s_isr_ts_out = isr_ts_out;
    s_boot_us = esp_timer_get_time();

    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIR_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = gpio_isr_handler_add(PIR_GPIO, pir_gpio_isr, NULL);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "PIR HC-SR501 inicializado en GPIO %d (flanco subida, warmup 5 s)", PIR_GPIO);
    }
    return err;
}

bool pir_sensor_is_active(void)
{
    return gpio_get_level(PIR_GPIO) != 0;
}
