#pragma once

#include "freertos/task.h"

CARDENZA_WEB_EXTERN_C_BEGIN

typedef void (*TimerCallbackFunction_t)(TimerHandle_t xTimer);
typedef void (*PendedFunction_t)(void*, uint32_t);

TimerHandle_t xTimerCreate(const char* const pcTimerName, const TickType_t xTimerPeriodInTicks, const BaseType_t xAutoReload,
                           void* const pvTimerID, TimerCallbackFunction_t pxCallbackFunction);
TimerHandle_t xTimerCreateStatic(const char* const pcTimerName, const TickType_t xTimerPeriodInTicks, const BaseType_t xAutoReload,
                                 void* const pvTimerID, TimerCallbackFunction_t pxCallbackFunction, StaticTimer_t* pxTimerBuffer);
void* pvTimerGetTimerID(const TimerHandle_t xTimer);
void vTimerSetTimerID(TimerHandle_t xTimer, void* pvNewID);
BaseType_t xTimerIsTimerActive(TimerHandle_t xTimer);
TaskHandle_t xTimerGetTimerDaemonTaskHandle(void);
BaseType_t xTimerPendFunctionCallFromISR(PendedFunction_t xFunctionToPend, void* pvParameter1, uint32_t ulParameter2,
                                         BaseType_t* pxHigherPriorityTaskWoken);
BaseType_t xTimerPendFunctionCall(PendedFunction_t xFunctionToPend, void* pvParameter1, uint32_t ulParameter2, TickType_t xTicksToWait);
const char* pcTimerGetName(TimerHandle_t xTimer);
void vTimerSetReloadMode(TimerHandle_t xTimer, const BaseType_t xAutoReload);
BaseType_t xTimerGetReloadMode(TimerHandle_t xTimer);
UBaseType_t uxTimerGetReloadMode(TimerHandle_t xTimer);
TickType_t xTimerGetPeriod(TimerHandle_t xTimer);
TickType_t xTimerGetExpiryTime(TimerHandle_t xTimer);
BaseType_t xTimerGetStaticBuffer(TimerHandle_t xTimer, StaticTimer_t** ppxTimerBuffer);

// The timer commands (FreeRTOS sends them to the daemon task; here they act at once).
BaseType_t xTimerStart(TimerHandle_t xTimer, TickType_t xTicksToWait);
BaseType_t xTimerStop(TimerHandle_t xTimer, TickType_t xTicksToWait);
BaseType_t xTimerChangePeriod(TimerHandle_t xTimer, TickType_t xNewPeriod, TickType_t xTicksToWait);
BaseType_t xTimerDelete(TimerHandle_t xTimer, TickType_t xTicksToWait);
BaseType_t xTimerReset(TimerHandle_t xTimer, TickType_t xTicksToWait);
BaseType_t xTimerStartFromISR(TimerHandle_t xTimer, BaseType_t* pxHigherPriorityTaskWoken);
BaseType_t xTimerStopFromISR(TimerHandle_t xTimer, BaseType_t* pxHigherPriorityTaskWoken);
BaseType_t xTimerChangePeriodFromISR(TimerHandle_t xTimer, TickType_t xNewPeriod, BaseType_t* pxHigherPriorityTaskWoken);
BaseType_t xTimerResetFromISR(TimerHandle_t xTimer, BaseType_t* pxHigherPriorityTaskWoken);

CARDENZA_WEB_EXTERN_C_END
