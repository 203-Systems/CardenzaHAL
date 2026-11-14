#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

CARDENZA_WEB_EXTERN_C_BEGIN
// The 2 MB PSRAM of the ESP32-S3R2, when the firmware's sdkconfig enables it (CONFIG_SPIRAM).
bool esp_psram_is_initialized(void);
size_t esp_psram_get_size(void);
esp_err_t esp_psram_init(void);
CARDENZA_WEB_EXTERN_C_END
