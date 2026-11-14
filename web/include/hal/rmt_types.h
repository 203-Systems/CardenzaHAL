#pragma once
#include <stdint.h>
typedef int rmt_clock_source_t;
#define RMT_CLK_SRC_DEFAULT 0
#define RMT_CLK_SRC_APB 0
#define RMT_CLK_SRC_XTAL 1
typedef union {
    struct {
        uint16_t duration0 : 15;
        uint16_t level0 : 1;
        uint16_t duration1 : 15;
        uint16_t level1 : 1;
    };
    uint32_t val;
} rmt_symbol_word_t;
