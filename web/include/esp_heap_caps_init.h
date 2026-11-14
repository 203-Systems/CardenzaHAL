#pragma once
#include "esp_err.h"
#include "esp_heap_caps.h"

CARDENZA_WEB_EXTERN_C_BEGIN
void heap_caps_init(void);
void heap_caps_enable_nonos_stack_heaps(void);
// caps[0..2] by priority, as ESP-IDF registers a heap; the web target ORs them.
esp_err_t heap_caps_add_region(intptr_t start, intptr_t end);
esp_err_t heap_caps_add_region_with_caps(const uint32_t caps[], intptr_t start, intptr_t end);
CARDENZA_WEB_EXTERN_C_END
