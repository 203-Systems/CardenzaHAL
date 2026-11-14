#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "web_compat.h"

CARDENZA_WEB_EXTERN_C_BEGIN
// Where a pointer points, by the web target's emulated RAMs: PSRAM blocks are "external", every
// other address (the firmware's statics, stacks, internal heap) counts as internal DRAM.
bool esp_ptr_external_ram(const void* p);
bool esp_ptr_internal(const void* p);
bool esp_ptr_dma_capable(const void* p);
bool esp_ptr_dma_ext_capable(const void* p);
bool esp_ptr_word_aligned(const void* p);
bool esp_ptr_executable(const void* p);
bool esp_ptr_byte_accessible(const void* p);
bool esp_ptr_in_dram(const void* p);
bool esp_ptr_in_iram(const void* p);
bool esp_ptr_in_drom(const void* p);
bool esp_ptr_in_rtc_slow(const void* p);
bool esp_ptr_in_rtc_dram_fast(const void* p);
bool esp_ptr_in_diram_dram(const void* p);
bool esp_stack_ptr_is_sane(uint32_t sp);
bool esp_ptr_in_visible_dram(const void* p);
CARDENZA_WEB_EXTERN_C_END
