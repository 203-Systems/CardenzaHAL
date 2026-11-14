#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "web_compat.h"

CARDENZA_WEB_EXTERN_C_BEGIN
typedef struct multi_heap_info* multi_heap_handle_t;
typedef struct {
    size_t total_free_bytes;
    size_t total_allocated_bytes;
    size_t largest_free_block;
    size_t minimum_free_bytes;
    size_t allocated_blocks;
    size_t free_blocks;
    size_t total_blocks;
} multi_heap_info_t;
CARDENZA_WEB_EXTERN_C_END
