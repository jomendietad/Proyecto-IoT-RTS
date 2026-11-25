#include "boot_button.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "status_led.h"

static const char *TAG = "boot_button";

#define BOOT_HOLD_RESET_US  (5000000LL)
#define BOOT_POLL_PERIOD_MS 100U

typedef struct {
    gpio_num_t gpio;
    boot_reset_cb_t cb;
} boot_btn_ctx_t;

static boot_btn_ctx_t s_ctx;
static TaskHandle_t s_task_handle = NULL;

static void task_boot_btn(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);

    int64_t press_start_us = 0;
    bool pressed = false;

    while (1) {
        const int level = gpio_get_level(s_ctx.gpio);
        const int64_t now_us = esp_timer_get_time();

        if (level == 0) {
            if (!pressed) {
                pressed = true;
                press_start_us = now_us;
            } else if ((now_us - press_start_us) >= BOOT_HOLD_RESET_US) {
                ESP_LOGW(TAG, "Factory reset activado por boton BOOT (5 s)");
                status_led_set_state(STATUS_LED_FACTORY_RESET);
                if (s_ctx.cb) {
                    s_ctx.cb();
                } else {
                    nvs_flash_erase();
                }
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        } else {
            pressed = false;
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_PERIOD_MS));
    }
}

esp_err_t boot_button_init(gpio_num_t gpio, boot_reset_cb_t on_factory_reset)
{
    if (s_task_handle != NULL) {
        return ESP_OK;
    }

    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    s_ctx.gpio = gpio;
    s_ctx.cb = on_factory_reset;

    BaseType_t ok = xTaskCreate(task_boot_btn, "task_boot_btn", 2560, NULL, 4, &s_task_handle);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
