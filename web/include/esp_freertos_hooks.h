#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
CARDENZA_WEB_EXTERN_C_BEGIN
typedef bool (*esp_freertos_idle_cb_t)(void);
typedef void (*esp_freertos_tick_cb_t)(void);
esp_err_t esp_register_freertos_idle_hook_for_cpu(esp_freertos_idle_cb_t new_idle_cb, UBaseType_t cpuid);
esp_err_t esp_register_freertos_idle_hook(esp_freertos_idle_cb_t new_idle_cb);
esp_err_t esp_register_freertos_tick_hook_for_cpu(esp_freertos_tick_cb_t new_tick_cb, UBaseType_t cpuid);
esp_err_t esp_register_freertos_tick_hook(esp_freertos_tick_cb_t new_tick_cb);
void esp_deregister_freertos_idle_hook_for_cpu(esp_freertos_idle_cb_t old_idle_cb, UBaseType_t cpuid);
void esp_deregister_freertos_idle_hook(esp_freertos_idle_cb_t old_idle_cb);
void esp_deregister_freertos_tick_hook_for_cpu(esp_freertos_tick_cb_t old_tick_cb, UBaseType_t cpuid);
void esp_deregister_freertos_tick_hook(esp_freertos_tick_cb_t old_tick_cb);
CARDENZA_WEB_EXTERN_C_END
