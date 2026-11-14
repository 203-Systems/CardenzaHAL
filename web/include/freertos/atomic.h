#pragma once
// FreeRTOS's atomic helpers: one thread runs everything, a critical section is not needed.
#include "freertos/FreeRTOS.h"
#define ATOMIC_COMPARE_AND_SWAP_SUCCESS 0x1U
#define ATOMIC_COMPARE_AND_SWAP_FAILURE 0x0U
static inline uint32_t Atomic_CompareAndSwap_u32(uint32_t volatile* pulDestination, uint32_t ulExchange, uint32_t ulComparand)
{ if (*pulDestination == ulComparand) { *pulDestination = ulExchange; return ATOMIC_COMPARE_AND_SWAP_SUCCESS; } return ATOMIC_COMPARE_AND_SWAP_FAILURE; }
static inline void* Atomic_SwapPointers_p32(void* volatile* ppvDestination, void* pvExchange) { void* old = *ppvDestination; *ppvDestination = pvExchange; return old; }
static inline uint32_t Atomic_Add_u32(uint32_t volatile* pulAddend, uint32_t ulCount) { uint32_t old = *pulAddend; *pulAddend += ulCount; return old; }
static inline uint32_t Atomic_Subtract_u32(uint32_t volatile* pulAddend, uint32_t ulCount) { uint32_t old = *pulAddend; *pulAddend -= ulCount; return old; }
static inline uint32_t Atomic_Increment_u32(uint32_t volatile* pulAddend) { return Atomic_Add_u32(pulAddend, 1); }
static inline uint32_t Atomic_Decrement_u32(uint32_t volatile* pulAddend) { return Atomic_Subtract_u32(pulAddend, 1); }
static inline uint32_t Atomic_OR_u32(uint32_t volatile* pulDestination, uint32_t ulValue) { uint32_t old = *pulDestination; *pulDestination |= ulValue; return old; }
static inline uint32_t Atomic_AND_u32(uint32_t volatile* pulDestination, uint32_t ulValue) { uint32_t old = *pulDestination; *pulDestination &= ulValue; return old; }
static inline uint32_t Atomic_XOR_u32(uint32_t volatile* pulDestination, uint32_t ulValue) { uint32_t old = *pulDestination; *pulDestination ^= ulValue; return old; }
