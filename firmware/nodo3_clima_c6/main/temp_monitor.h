#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "iot_common_types.h"
#include "ble_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t             temp_deci_c;
    int16_t             delta_deci_c;
    iot_temp_alert_t    alert_state;
    ble_clima_reason_t  reason;
    uint8_t             threshold_high_c;
    bool                should_notify;
} temp_eval_result_t;

esp_err_t temp_monitor_init(void);

esp_err_t temp_monitor_set_threshold(uint8_t t_high_c);

void temp_monitor_evaluate(int16_t temp_deci_c, temp_eval_result_t *out);

void temp_monitor_get_state(iot_temp_alert_t *out_alert,
                            uint8_t *out_t_high_c,
                            int16_t *out_last_temp_deci,
                            int16_t *out_last_delta_deci);

#ifdef __cplusplus
}
#endif
