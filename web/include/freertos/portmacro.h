#pragma once

// The port: types, critical sections and core identity as ESP-IDF's Xtensa port has them.

#include <stdint.h>
#include <stdbool.h>

#include "web_compat.h"

CARDENZA_WEB_EXTERN_C_BEGIN

typedef uint8_t StackType_t;  // ESP-IDF: stack depths are in bytes
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define portTICK_PERIOD_MS ((TickType_t)1000 / configTICK_RATE_HZ)
#define portTICK_RATE_MS portTICK_PERIOD_MS
#define portBYTE_ALIGNMENT 16
#define portSTACK_GROWTH (-1)
#define portNUM_PROCESSORS 2
#define portCRITICAL_NESTING_IN_TCB 0
#define portUSING_MPU_WRAPPERS 0
#define portPRIVILEGE_BIT 0
#define portTICK_TYPE_IS_ATOMIC 1

// A spinlock between the two cores. On the web one thread runs everything: entering one only
// stops the scheduler from switching tasks until it is left.
typedef struct {
    uint32_t owner;
    uint32_t count;
} spinlock_t;
typedef spinlock_t portMUX_TYPE;
#define SPINLOCK_FREE 0xB33FFFFF
#define SPINLOCK_WAIT_FOREVER (-1)
#define SPINLOCK_NO_WAIT 0
#define SPINLOCK_INITIALIZER {.owner = SPINLOCK_FREE, .count = 0}
#define portMUX_FREE_VAL SPINLOCK_FREE
#define portMUX_NO_TIMEOUT SPINLOCK_WAIT_FOREVER
#define portMUX_TRY_LOCK SPINLOCK_NO_WAIT
#define portMUX_INITIALIZER_UNLOCKED SPINLOCK_INITIALIZER

void spinlock_initialize(spinlock_t* lock);
bool spinlock_acquire(spinlock_t* lock, int32_t timeout);
void spinlock_release(spinlock_t* lock);
void vPortEnterCritical(portMUX_TYPE* mux);
void vPortExitCritical(portMUX_TYPE* mux);
BaseType_t xPortEnterCriticalTimeout(portMUX_TYPE* mux, BaseType_t timeout);
void vPortEnterCriticalSafe(portMUX_TYPE* mux);
void vPortExitCriticalSafe(portMUX_TYPE* mux);
BaseType_t xPortInIsrContext(void);
BaseType_t xPortInterruptedFromISRContext(void);
BaseType_t xPortGetCoreID(void);
void vPortYield(void);
void vPortYieldFromISR(void);
void vPortYieldOtherCore(BaseType_t coreid);
UBaseType_t xPortSetInterruptMaskFromISR(void);
void vPortClearInterruptMaskFromISR(UBaseType_t prev_level);
uint32_t xPortGetTickRateHz(void);
void vPortSetStackWatchpoint(void* pxStackStart);
BaseType_t xPortCheckValidTCBMem(const void* ptr);
BaseType_t xPortcheckValidStackMem(const void* ptr);

#define portMUX_INITIALIZE(mux) spinlock_initialize(mux)
#define portTRY_ENTER_CRITICAL(mux, timeout) xPortEnterCriticalTimeout(mux, timeout)
#define portENTER_CRITICAL(mux) vPortEnterCritical(mux)
#define portEXIT_CRITICAL(mux) vPortExitCritical(mux)
#define portTRY_ENTER_CRITICAL_ISR(mux, timeout) xPortEnterCriticalTimeout(mux, timeout)
#define portENTER_CRITICAL_ISR(mux) vPortEnterCritical(mux)
#define portEXIT_CRITICAL_ISR(mux) vPortExitCritical(mux)
#define portTRY_ENTER_CRITICAL_SAFE(mux, timeout) xPortEnterCriticalTimeout(mux, timeout)
#define portENTER_CRITICAL_SAFE(mux) vPortEnterCriticalSafe(mux)
#define portEXIT_CRITICAL_SAFE(mux) vPortExitCriticalSafe(mux)
#define taskENTER_CRITICAL(mux) portENTER_CRITICAL(mux)
#define taskEXIT_CRITICAL(mux) portEXIT_CRITICAL(mux)
#define taskENTER_CRITICAL_ISR(mux) portENTER_CRITICAL_ISR(mux)
#define taskEXIT_CRITICAL_ISR(mux) portEXIT_CRITICAL_ISR(mux)
#define taskENTER_CRITICAL_SAFE(mux) portENTER_CRITICAL_SAFE(mux)
#define taskEXIT_CRITICAL_SAFE(mux) portEXIT_CRITICAL_SAFE(mux)
#define portSET_INTERRUPT_MASK_FROM_ISR() xPortSetInterruptMaskFromISR()
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(prev_level) vPortClearInterruptMaskFromISR(prev_level)
#define portDISABLE_INTERRUPTS() xPortSetInterruptMaskFromISR()
#define portENABLE_INTERRUPTS() vPortClearInterruptMaskFromISR(0)
#define portYIELD() vPortYield()
#define portYIELD_WITHIN_API() vPortYield()
#define portYIELD_FROM_ISR_NO_ARG() vPortYieldFromISR()
#define portYIELD_FROM_ISR_ARG(x) do { if (x) vPortYieldFromISR(); } while (0)
#define portGET_ARGUMENT_COUNT_INNER(zero, one, count, ...) count
#define portGET_ARGUMENT_COUNT(...) portGET_ARGUMENT_COUNT_INNER(0, ##__VA_ARGS__, 1, 0)
#define portYIELD_FROM_ISR_SELECT(count) portYIELD_FROM_ISR_SELECT_##count
#define portYIELD_FROM_ISR_SELECT_0 portYIELD_FROM_ISR_NO_ARG
#define portYIELD_FROM_ISR_SELECT_1 portYIELD_FROM_ISR_ARG
#define portYIELD_FROM_ISR_PICK(count) portYIELD_FROM_ISR_SELECT(count)
#define portYIELD_FROM_ISR(...) portYIELD_FROM_ISR_PICK(portGET_ARGUMENT_COUNT(__VA_ARGS__))(__VA_ARGS__)
#define portEND_SWITCHING_ISR(x) portYIELD_FROM_ISR_ARG(x)
#define portYIELD_CORE(x) vPortYieldOtherCore(x)
#define portGET_CORE_ID() xPortGetCoreID()
#define portCHECK_IF_IN_ISR() xPortInIsrContext()
#define portNOP() __asm__ volatile("nop")
#define portASSERT_IF_IN_ISR() configASSERT(!xPortInIsrContext())
#define portVALID_TCB_MEM(ptr) xPortCheckValidTCBMem(ptr)
#define portVALID_STACK_MEM(ptr) xPortcheckValidStackMem(ptr)
#define portTASK_FUNCTION_PROTO(vFunction, pvParameters) void vFunction(void* pvParameters)
#define portTASK_FUNCTION(vFunction, pvParameters) void vFunction(void* pvParameters)


CARDENZA_WEB_EXTERN_C_END
