#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_attr.h"
#include "esp_bit_defs.h"
#include "esp_idf_version.h"
#include "sdkconfig.h"

CARDENZA_WEB_EXTERN_C_BEGIN

typedef enum {
    ESP_RST_UNKNOWN,
    ESP_RST_POWERON,
    ESP_RST_EXT,
    ESP_RST_SW,
    ESP_RST_PANIC,
    ESP_RST_INT_WDT,
    ESP_RST_TASK_WDT,
    ESP_RST_WDT,
    ESP_RST_DEEPSLEEP,
    ESP_RST_BROWNOUT,
    ESP_RST_SDIO,
    ESP_RST_USB,
    ESP_RST_JTAG,
    ESP_RST_EFUSE,
    ESP_RST_PWR_GLITCH,
    ESP_RST_CPU_LOCKUP,
} esp_reset_reason_t;

typedef void (*shutdown_handler_t)(void);

esp_err_t esp_register_shutdown_handler(shutdown_handler_t handle);
esp_err_t esp_unregister_shutdown_handler(shutdown_handler_t handle);
// The simulator restarts the firmware (a new instance of the module; the page stays).
void esp_restart(void) __attribute__((noreturn));
esp_reset_reason_t esp_reset_reason(void);
uint32_t esp_get_free_heap_size(void);
uint32_t esp_get_free_internal_heap_size(void);
uint32_t esp_get_minimum_free_heap_size(void);
void esp_system_abort(const char* details) __attribute__((noreturn));
const char* esp_get_idf_version(void);

CARDENZA_WEB_EXTERN_C_END

#include "esp_chip_info.h"
#include "esp_random.h"
#include "esp_mac.h"
