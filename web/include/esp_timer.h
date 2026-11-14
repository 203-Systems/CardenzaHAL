#pragma once

// esp_timer: microseconds since boot and one-shot / periodic callbacks, dispatched from the
// "esp_timer" task (priority 22) or, with ESP_TIMER_ISR, in ISR context.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

CARDENZA_WEB_EXTERN_C_BEGIN

typedef struct esp_timer* esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void* arg);
typedef enum {
    ESP_TIMER_TASK,
    ESP_TIMER_ISR,
    ESP_TIMER_MAX,
} esp_timer_dispatch_t;

typedef struct {
    esp_timer_cb_t callback;
    void* arg;
    esp_timer_dispatch_t dispatch_method;
    const char* name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;

esp_err_t esp_timer_early_init(void);
esp_err_t esp_timer_init(void);
esp_err_t esp_timer_deinit(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t* create_args, esp_timer_handle_t* out_handle);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period);
esp_err_t esp_timer_restart(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);
int64_t esp_timer_get_time(void);
int64_t esp_timer_get_next_alarm(void);
int64_t esp_timer_get_next_alarm_for_wake_up(void);
esp_err_t esp_timer_get_period(esp_timer_handle_t timer, uint64_t* period);
esp_err_t esp_timer_get_expiry_time(esp_timer_handle_t timer, uint64_t* expiry);
esp_err_t esp_timer_dump(FILE* stream);
void esp_timer_isr_dispatch_need_yield(void);
bool esp_timer_is_active(esp_timer_handle_t timer);

CARDENZA_WEB_EXTERN_C_END
