#pragma once

#include "esp_err.h"
#include "iot_common_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t lock_ctrl_init(void);

esp_err_t lock_ctrl_set(iot_lock_state_t state);

iot_lock_state_t lock_ctrl_toggle(void);

iot_lock_state_t lock_ctrl_get(void);

#ifdef __cplusplus
}
#endif
