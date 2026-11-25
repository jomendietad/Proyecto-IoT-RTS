#include "alarm_fsm.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "ble_protocol.h"
#include "pir_sensor.h"
#include "status_led.h"

static const char *TAG = "alarm_fsm";

#define ALARM_LED_GPIO      GPIO_NUM_20
#define ALARM_DEADLINE_US   10000U /* D = 10 ms según diseno_rtos_str.md */

typedef enum {
    BLINK_MODE_IDLE = 0,
    BLINK_MODE_DISARM_1FLASH,
    BLINK_MODE_ARM_2FLASH,
    BLINK_MODE_TRIGGERED_CONT,
} blink_mode_t;

static str_metrics_ctx_t *s_metrics = NULL;
static TaskHandle_t s_task_handle = NULL;
static SemaphoreHandle_t s_state_mutex = NULL;
static TimerHandle_t s_blink_timer = NULL;

static iot_alarm_state_t s_state = ALARM_STATE_DESARMADA;
static uint8_t s_fail_counter = 0;
static uint8_t s_seq = 0;
static volatile int64_t s_pir_isr_ts_us = 0;

static blink_mode_t s_blink_mode = BLINK_MODE_IDLE;
static uint8_t s_blink_step = 0;
static bool s_led_level = false;

static void set_alarm_led(bool on)
{
    s_led_level = on;
    gpio_set_level(ALARM_LED_GPIO, on ? 1 : 0);
}

static void blink_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    switch (s_blink_mode) {
    case BLINK_MODE_DISARM_1FLASH:
        set_alarm_led(false);
        s_blink_mode = BLINK_MODE_IDLE;
        xTimerStop(s_blink_timer, 0);
        break;

    case BLINK_MODE_ARM_2FLASH:
        s_blink_step++;
        if (s_blink_step == 1U) {
            set_alarm_led(false);
        } else if (s_blink_step == 2U) {
            set_alarm_led(true);
        } else {
            set_alarm_led(false);
            s_blink_mode = BLINK_MODE_IDLE;
            xTimerStop(s_blink_timer, 0);
        }
        break;

    case BLINK_MODE_TRIGGERED_CONT:
        set_alarm_led(!s_led_level);
        break;

    case BLINK_MODE_IDLE:
    default:
        xTimerStop(s_blink_timer, 0);
        break;
    }
}

static void start_pattern_disarmed(void)
{
    xTimerStop(s_blink_timer, 0);
    s_blink_mode = BLINK_MODE_DISARM_1FLASH;
    s_blink_step = 0;
    set_alarm_led(true);
    xTimerChangePeriod(s_blink_timer, pdMS_TO_TICKS(150), 0);
    xTimerStart(s_blink_timer, 0);
}

static void start_pattern_armed(void)
{
    xTimerStop(s_blink_timer, 0);
    s_blink_mode = BLINK_MODE_ARM_2FLASH;
    s_blink_step = 0;
    set_alarm_led(true);
    xTimerChangePeriod(s_blink_timer, pdMS_TO_TICKS(100), 0);
    xTimerStart(s_blink_timer, 0);
}

static void start_pattern_triggered(void)
{
    xTimerStop(s_blink_timer, 0);
    s_blink_mode = BLINK_MODE_TRIGGERED_CONT;
    s_blink_step = 0;
    set_alarm_led(true);
    xTimerChangePeriod(s_blink_timer, pdMS_TO_TICKS(150), 0);
    xTimerStart(s_blink_timer, 0);
}

static void log_alarm_ble_event(iot_alarm_state_t st, iot_alarm_reason_t reason, uint32_t lat_us)
{
    const ble_alarm_event_t pkt = {
        .seq_num = ++s_seq,
        .new_state = (uint8_t)st,
        .reason = (uint8_t)reason,
        .local_latency_us = lat_us,
        .fail_counter = s_fail_counter,
        .flags = pir_sensor_is_active() ? 0x01U : 0x00U,
    };
    ESP_LOGI(TAG, "FSM -> %s (reason=%u fail=%u lat=%" PRIu32 "us) | GATT Notify: OP_ALARM_EVENT seq=%u",
             iot_alarm_state_str(st), pkt.reason, pkt.fail_counter, pkt.local_latency_us, pkt.seq_num);
}

static void task_alarm_fsm(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);

    uint32_t bits = 0;
    while (1) {
        if (xTaskNotifyWait(0, 0xFFFFFFFFUL, &bits, pdMS_TO_TICKS(2000)) == pdTRUE) {
            if (bits & ALARM_EVT_CMD_DISARM) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                s_state = ALARM_STATE_DESARMADA;
                s_fail_counter = 0;
                start_pattern_disarmed();
                xSemaphoreGive(s_state_mutex);

                status_led_set_state(STATUS_LED_UNPROVISIONED);
                log_alarm_ble_event(ALARM_STATE_DESARMADA, ALARM_REASON_CMD_TG, 0);
            }

            if (bits & ALARM_EVT_CMD_ARM) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                if (s_state != ALARM_STATE_DISPARADA) {
                    s_state = ALARM_STATE_ARMADA;
                    s_fail_counter = 0;
                    start_pattern_armed();
                    xSemaphoreGive(s_state_mutex);
                    log_alarm_ble_event(ALARM_STATE_ARMADA, ALARM_REASON_CMD_TG, 0);
                } else {
                    xSemaphoreGive(s_state_mutex);
                }
            }

            if (bits & ALARM_EVT_PIR_MOTION) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                if (s_state == ALARM_STATE_ARMADA) {
                    /* Encendido inmediato de LED antes de logging para WCRT mínimo */
                    set_alarm_led(true);
                    const uint32_t lat_us = (uint32_t)(esp_timer_get_time() - s_pir_isr_ts_us);
                    s_state = ALARM_STATE_DISPARADA;
                    start_pattern_triggered();
                    xSemaphoreGive(s_state_mutex);

                    status_led_set_state(STATUS_LED_ALARM_TRIGGERED);
                    str_metrics_record(s_metrics, "pir_to_led_us", lat_us, ALARM_DEADLINE_US, false);
                    log_alarm_ble_event(ALARM_STATE_DISPARADA, ALARM_REASON_PIR_MOTION, lat_us);
                } else {
                    const iot_alarm_state_t cur = s_state;
                    xSemaphoreGive(s_state_mutex);
                    ESP_LOGI(TAG, "Movimiento PIR detectado (estado=%s, sin disparo)", iot_alarm_state_str(cur));
                }
            }

            if (bits & ALARM_EVT_RFID_FAIL) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                if (s_state == ALARM_STATE_ARMADA) {
                    s_fail_counter++;
                    if (s_fail_counter >= IOT_RFID_FAIL_TRIGGER_COUNT) {
                        s_state = ALARM_STATE_DISPARADA;
                        start_pattern_triggered();
                        xSemaphoreGive(s_state_mutex);
                        status_led_set_state(STATUS_LED_ALARM_TRIGGERED);
                        log_alarm_ble_event(ALARM_STATE_DISPARADA, ALARM_REASON_RFID_3_FAILS, 0);
                    } else {
                        const uint8_t cur_fails = s_fail_counter;
                        xSemaphoreGive(s_state_mutex);
                        ESP_LOGW(TAG, "Fallo RFID acumulado (%u/%u) con alarma ARMADA",
                                 cur_fails, IOT_RFID_FAIL_TRIGGER_COUNT);
                    }
                } else {
                    xSemaphoreGive(s_state_mutex);
                }
            }
        }

        esp_task_wdt_reset();
    }
}

esp_err_t alarm_fsm_init(str_metrics_ctx_t *metrics, TaskHandle_t *out_task_handle)
{
    s_metrics = metrics;
    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) {
        return ESP_ERR_NO_MEM;
    }

    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << ALARM_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    set_alarm_led(false);

    s_blink_timer = xTimerCreate("alarm_blink", pdMS_TO_TICKS(150), pdTRUE, NULL, blink_timer_cb);
    if (!s_blink_timer) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(task_alarm_fsm, "task_alarm_fsm", 4096, NULL, 16, &s_task_handle);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (out_task_handle) {
        *out_task_handle = s_task_handle;
    }

    return pir_sensor_init(s_task_handle, ALARM_EVT_PIR_MOTION, &s_pir_isr_ts_us);
}

void alarm_fsm_post_event(uint32_t evt_bit)
{
    if (s_task_handle) {
        xTaskNotify(s_task_handle, evt_bit, eSetBits);
    }
}

void alarm_fsm_trigger_sim_pir(void)
{
    s_pir_isr_ts_us = esp_timer_get_time();
    alarm_fsm_post_event(ALARM_EVT_PIR_MOTION);
}

iot_alarm_state_t alarm_fsm_get_state(void)
{
    if (!s_state_mutex) {
        return ALARM_STATE_DESARMADA;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const iot_alarm_state_t st = s_state;
    xSemaphoreGive(s_state_mutex);
    return st;
}

uint8_t alarm_fsm_get_fail_counter(void)
{
    if (!s_state_mutex) {
        return 0;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const uint8_t fc = s_fail_counter;
    xSemaphoreGive(s_state_mutex);
    return fc;
}
