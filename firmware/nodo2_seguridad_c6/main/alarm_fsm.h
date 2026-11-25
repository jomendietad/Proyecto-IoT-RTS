#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_common_types.h"
#include "str_metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALARM_EVT_PIR_MOTION  (1UL << 0)
#define ALARM_EVT_CMD_ARM     (1UL << 1)
#define ALARM_EVT_CMD_DISARM  (1UL << 2)
#define ALARM_EVT_RFID_FAIL   (1UL << 3)
#define ALARM_EVT_RFID_ARM    (1UL << 4)
#define ALARM_EVT_RFID_DISARM (1UL << 5)

esp_err_t alarm_fsm_init(str_metrics_ctx_t *metrics, TaskHandle_t *out_task_handle);

void alarm_fsm_post_event(uint32_t evt_bit);

void alarm_fsm_trigger_sim_pir(void);

iot_alarm_state_t alarm_fsm_get_state(void);

uint8_t alarm_fsm_get_fail_counter(void);

void alarm_fsm_send_current_status(void);

#ifdef __cplusplus
}
#endif
