#pragma once

// Where things go in the chip's memories. On the web there is one memory: the attributes keep
// their meaning for the build (section names) and change nothing at run time.
#include "esp_bit_defs.h"
#include "esp_compiler.h"

#define IRAM_ATTR
#define DRAM_ATTR
#define IRAM_DATA_ATTR
#define IRAM_BSS_ATTR
#define DRAM_DMA_ALIGNED_ATTR __attribute__((aligned(4)))
#define DMA_ATTR __attribute__((aligned(4)))
#define FLASH_ATTR
#define WORD_ALIGNED_ATTR __attribute__((aligned(4)))
#define RTC_IRAM_ATTR
#define RTC_DATA_ATTR
#define RTC_RODATA_ATTR
#define RTC_SLOW_ATTR
#define RTC_FAST_ATTR
#define RTC_NOINIT_ATTR
#define RTC_DATA_ATTR_MEM
#define COREDUMP_DRAM_ATTR
#define COREDUMP_IRAM_DATA_ATTR
#define EXT_RAM_ATTR
#define EXT_RAM_BSS_ATTR
#define EXT_RAM_NOINIT_ATTR
#define NOINIT_ATTR
#define DRAM_STR(str) (str)
#define IRAM_STR(str) (str)
#define FORCE_INLINE_ATTR static inline __attribute__((always_inline))
#define NOINLINE_ATTR __attribute__((noinline))
#define TCM_IRAM_ATTR
#define TCM_DRAM_ATTR
#define ESP_ALWAYS_INLINE
#define _SECTION_ATTR_IMPL(SECTION, COUNTER)
#define __NOINIT_ATTR

#ifdef __cplusplus
#define FLAG_ATTR(TYPE) \
    constexpr TYPE operator~(TYPE a) { return (TYPE) ~(int)a; } \
    constexpr TYPE operator|(TYPE a, TYPE b) { return (TYPE)((int)a | (int)b); } \
    constexpr TYPE operator&(TYPE a, TYPE b) { return (TYPE)((int)a & (int)b); } \
    constexpr TYPE operator^(TYPE a, TYPE b) { return (TYPE)((int)a ^ (int)b); } \
    constexpr TYPE operator>>(TYPE a, int b) { return (TYPE)((int)a >> b); } \
    constexpr TYPE operator<<(TYPE a, int b) { return (TYPE)((int)a << b); } \
    TYPE& operator|=(TYPE& a, TYPE b) { a = a | b; return a; } \
    TYPE& operator&=(TYPE& a, TYPE b) { a = a & b; return a; } \
    TYPE& operator^=(TYPE& a, TYPE b) { a = a ^ b; return a; } \
    TYPE& operator>>=(TYPE& a, int b) { a = a >> b; return a; } \
    TYPE& operator<<=(TYPE& a, int b) { a = a << b; return a; }
#else
#define FLAG_ATTR(TYPE)
#endif
