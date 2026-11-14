#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "hal/timer_types.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct gptimer_t* gptimer_handle_t;
typedef struct {
    uint64_t count_value;
    uint64_t alarm_value;
} gptimer_alarm_event_data_t;
typedef bool (*gptimer_alarm_cb_t)(gptimer_handle_t timer, const gptimer_alarm_event_data_t* edata, void* user_ctx);
#ifdef __cplusplus
}
#endif
