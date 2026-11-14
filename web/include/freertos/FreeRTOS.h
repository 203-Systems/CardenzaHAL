#pragma once

// FreeRTOS as ESP-IDF 5.3 has it (IDF FreeRTOS, kernel v10.5, two cores), on the CardenzaHAL web
// target: the scheduler is web/src/kernel (one thread, cooperative at every API call).

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#include "sdkconfig.h"
#include "web_compat.h"
#include "freertos/projdefs.h"
#include "freertos/portmacro.h"

#ifndef CONFIG_FREERTOS_HZ
#define CONFIG_FREERTOS_HZ 1000
#endif
#ifndef CONFIG_FREERTOS_MAX_TASK_NAME_LEN
#define CONFIG_FREERTOS_MAX_TASK_NAME_LEN 16
#endif
#ifndef CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES
#define CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES 1
#endif
#ifndef CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS
#define CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS 1
#endif
#ifndef CONFIG_FREERTOS_TIMER_TASK_PRIORITY
#define CONFIG_FREERTOS_TIMER_TASK_PRIORITY 1
#endif
#ifndef CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH
#define CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH 2048
#endif
#ifndef CONFIG_FREERTOS_TIMER_QUEUE_LENGTH
#define CONFIG_FREERTOS_TIMER_QUEUE_LENGTH 10
#endif
#ifndef CONFIG_FREERTOS_IDLE_TASK_STACKSIZE
#define CONFIG_FREERTOS_IDLE_TASK_STACKSIZE 1536
#endif
#ifndef CONFIG_FREERTOS_NUMBER_OF_CORES
#define CONFIG_FREERTOS_NUMBER_OF_CORES 2
#endif

#define configTICK_RATE_HZ CONFIG_FREERTOS_HZ
#define configMAX_PRIORITIES (25)
#define configMINIMAL_STACK_SIZE (CONFIG_FREERTOS_IDLE_TASK_STACKSIZE)
#define configMAX_TASK_NAME_LEN CONFIG_FREERTOS_MAX_TASK_NAME_LEN
#define configTASK_NOTIFICATION_ARRAY_ENTRIES CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS
#define configTIMER_TASK_PRIORITY CONFIG_FREERTOS_TIMER_TASK_PRIORITY
#define configTIMER_TASK_STACK_DEPTH CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH
#define configTIMER_QUEUE_LENGTH CONFIG_FREERTOS_TIMER_QUEUE_LENGTH
#define configNUMBER_OF_CORES CONFIG_FREERTOS_NUMBER_OF_CORES
#define configUSE_PREEMPTION 1
#define configUSE_TIME_SLICING 1
#define configUSE_MUTEXES 1
#define configUSE_RECURSIVE_MUTEXES 1
#define configUSE_COUNTING_SEMAPHORES 1
#define configUSE_TIMERS 1
#define configUSE_QUEUE_SETS 1
#define configUSE_TASK_NOTIFICATIONS 1
#define configSUPPORT_STATIC_ALLOCATION 1
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configUSE_TRACE_FACILITY 1
#define configGENERATE_RUN_TIME_STATS 1
#define configUSE_16_BIT_TICKS 0
#define configSTACK_DEPTH_TYPE uint32_t
#define configASSERT(x) do { if (!(x)) vAssertCalled(__FILE__, __LINE__); } while (0)
#define INCLUDE_vTaskDelete 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_vTaskDelay 1
#define INCLUDE_xTaskDelayUntil 1
#define INCLUDE_uxTaskPriorityGet 1
#define INCLUDE_vTaskPrioritySet 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xTaskGetIdleTaskHandle 1
#define INCLUDE_eTaskGetState 1
#define INCLUDE_xTaskAbortDelay 1
#define INCLUDE_xTaskGetHandle 1
#define INCLUDE_xTimerPendFunctionCall 1
#define INCLUDE_xSemaphoreGetMutexHolder 1

CARDENZA_WEB_EXTERN_C_BEGIN

typedef void (*TaskFunction_t)(void*);
typedef struct tskTaskControlBlock* TaskHandle_t;
typedef struct QueueDefinition* QueueHandle_t;
typedef struct QueueDefinition* QueueSetHandle_t;
typedef struct QueueDefinition* QueueSetMemberHandle_t;
typedef struct tmrTimerControl* TimerHandle_t;
typedef struct EventGroupDef_t* EventGroupHandle_t;
typedef struct StreamBufferDef_t* StreamBufferHandle_t;
typedef StreamBufferHandle_t MessageBufferHandle_t;
typedef TickType_t EventBits_t;

// Room for the kernel's objects, so the ...Static calls need no heap.
typedef struct { uint8_t storage[512]; } StaticTask_t;
typedef struct { uint8_t storage[192]; } StaticQueue_t;
typedef StaticQueue_t StaticSemaphore_t;
typedef struct { uint8_t storage[96]; } StaticTimer_t;
typedef struct { uint8_t storage[64]; } StaticEventGroup_t;
typedef struct { uint8_t storage[96]; } StaticStreamBuffer_t;
typedef StaticStreamBuffer_t StaticMessageBuffer_t;

void vAssertCalled(const char* file, int line);

CARDENZA_WEB_EXTERN_C_END
