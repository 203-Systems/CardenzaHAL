#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
CARDENZA_WEB_EXTERN_C_BEGIN
// The task watchdog: accepted and fed, never fires (the web target warns about a task that holds the
// one thread for half a second instead).
typedef struct { uint32_t timeout_ms; uint32_t idle_core_mask; bool trigger_panic; } esp_task_wdt_config_t;
typedef struct esp_task_wdt_user_handle_s* esp_task_wdt_user_handle_t;
esp_err_t esp_task_wdt_init(const esp_task_wdt_config_t* config);
esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t* config);
esp_err_t esp_task_wdt_deinit(void);
esp_err_t esp_task_wdt_add(TaskHandle_t task_handle);
esp_err_t esp_task_wdt_add_user(const char* user_name, esp_task_wdt_user_handle_t* user_handle_ret);
esp_err_t esp_task_wdt_reset(void);
esp_err_t esp_task_wdt_reset_user(esp_task_wdt_user_handle_t user_handle);
esp_err_t esp_task_wdt_delete(TaskHandle_t task_handle);
esp_err_t esp_task_wdt_delete_user(esp_task_wdt_user_handle_t user_handle);
esp_err_t esp_task_wdt_status(TaskHandle_t task_handle);
esp_err_t esp_task_wdt_print_triggered_tasks(void* msg_handler, void* opaque, int* cpus_fail);
CARDENZA_WEB_EXTERN_C_END
