#pragma once
#include <stddef.h>
#include <stdint.h>
#include "web_compat.h"
CARDENZA_WEB_EXTERN_C_BEGIN
uint32_t esp_random(void);
void esp_fill_random(void* buf, size_t len);
CARDENZA_WEB_EXTERN_C_END
