#pragma once

// ESP-IDF's heap with capabilities, on the web target's two emulated RAMs: internal RAM (with the
// device's room, web/src/esp/heap.cpp) and the 2 MB PSRAM (when the firmware's sdkconfig has it).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "multi_heap.h"
#include "sdkconfig.h"

CARDENZA_WEB_EXTERN_C_BEGIN

#define MALLOC_CAP_EXEC (1 << 0)
#define MALLOC_CAP_32BIT (1 << 1)
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_DMA (1 << 3)
#define MALLOC_CAP_PID2 (1 << 4)
#define MALLOC_CAP_PID3 (1 << 5)
#define MALLOC_CAP_PID4 (1 << 6)
#define MALLOC_CAP_PID5 (1 << 7)
#define MALLOC_CAP_PID6 (1 << 8)
#define MALLOC_CAP_PID7 (1 << 9)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_DEFAULT (1 << 12)
#define MALLOC_CAP_IRAM_8BIT (1 << 13)
#define MALLOC_CAP_RETENTION (1 << 14)
#define MALLOC_CAP_RTCRAM (1 << 15)
#define MALLOC_CAP_TCM (1 << 16)
#define MALLOC_CAP_DMA_DESC_AHB (1 << 17)
#define MALLOC_CAP_DMA_DESC_AXI (1 << 18)
#define MALLOC_CAP_CACHE_ALIGNED (1 << 19)
#define MALLOC_CAP_SIMD (1 << 20)
#define MALLOC_CAP_INVALID (1u << 31)

typedef void (*esp_alloc_failed_hook_t)(size_t size, uint32_t caps, const char* function_name);
esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback);
void* heap_caps_malloc(size_t size, uint32_t caps);
void heap_caps_free(void* ptr);
void* heap_caps_realloc(void* ptr, size_t size, uint32_t caps);
void* heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
void heap_caps_aligned_free(void* ptr);
void* heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, uint32_t caps);
void* heap_caps_calloc(size_t n, size_t size, uint32_t caps);
size_t heap_caps_get_total_size(uint32_t caps);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
esp_err_t heap_caps_monitor_local_minimum_free_size_start(void);
esp_err_t heap_caps_monitor_local_minimum_free_size_stop(void);
void heap_caps_get_info(multi_heap_info_t* info, uint32_t caps);
void heap_caps_print_heap_info(uint32_t caps);
bool heap_caps_check_integrity_all(bool print_errors);
bool heap_caps_check_integrity(uint32_t caps, bool print_errors);
bool heap_caps_check_integrity_addr(intptr_t addr, bool print_errors);
void heap_caps_malloc_extmem_enable(size_t limit);
void* heap_caps_malloc_prefer(size_t size, size_t num, ...);
void* heap_caps_realloc_prefer(void* ptr, size_t size, size_t num, ...);
void* heap_caps_calloc_prefer(size_t n, size_t size, size_t num, ...);
void heap_caps_dump(uint32_t caps);
void heap_caps_dump_all(void);
size_t heap_caps_get_allocated_size(void* ptr);

CARDENZA_WEB_EXTERN_C_END
