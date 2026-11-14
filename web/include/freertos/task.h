#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/list.h"

CARDENZA_WEB_EXTERN_C_BEGIN

#define tskKERNEL_VERSION_NUMBER "V10.5.1"
#define tskKERNEL_VERSION_MAJOR 10
#define tskKERNEL_VERSION_MINOR 5
#define tskKERNEL_VERSION_BUILD 1
#define tskIDLE_PRIORITY ((UBaseType_t)0U)
#define tskNO_AFFINITY ((BaseType_t)0x7FFFFFFF)
#define tskDEFAULT_INDEX_TO_NOTIFY (0)
#define taskSCHEDULER_SUSPENDED ((BaseType_t)0)
#define taskSCHEDULER_NOT_STARTED ((BaseType_t)1)
#define taskSCHEDULER_RUNNING ((BaseType_t)2)
#define taskYIELD() portYIELD()
#define taskDISABLE_INTERRUPTS() portDISABLE_INTERRUPTS()
#define taskENABLE_INTERRUPTS() portENABLE_INTERRUPTS()

typedef enum { eRunning = 0, eReady, eBlocked, eSuspended, eDeleted, eInvalid } eTaskState;
typedef enum { eNoAction = 0, eSetBits, eIncrement, eSetValueWithOverwrite, eSetValueWithoutOverwrite } eNotifyAction;
typedef enum { eAbortSleep = 0, eStandardSleep, eNoTasksWaitingTimeout } eSleepModeStatus;

typedef struct xTIME_OUT {
    BaseType_t xOverflowCount;
    TickType_t xTimeOnEntering;
} TimeOut_t;

typedef struct xTASK_STATUS {
    TaskHandle_t xHandle;
    const char* pcTaskName;
    UBaseType_t xTaskNumber;
    eTaskState eCurrentState;
    UBaseType_t uxCurrentPriority;
    UBaseType_t uxBasePriority;
    uint32_t ulRunTimeCounter;
    StackType_t* pxStackBase;
    configSTACK_DEPTH_TYPE usStackHighWaterMark;
    BaseType_t xCoreID;
} TaskStatus_t;

typedef BaseType_t (*TaskHookFunction_t)(void*);
typedef void (*TlsDeleteCallbackFunction_t)(int, void*);

// Creation and deletion
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t pxTaskCode, const char* const pcName, const configSTACK_DEPTH_TYPE usStackDepth,
                                   void* const pvParameters, UBaseType_t uxPriority, TaskHandle_t* const pxCreatedTask,
                                   const BaseType_t xCoreID);
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t pxTaskCode, const char* const pcName, const uint32_t ulStackDepth,
                                           void* const pvParameters, UBaseType_t uxPriority, StackType_t* const puxStackBuffer,
                                           StaticTask_t* const pxTaskBuffer, const BaseType_t xCoreID);
static inline BaseType_t xTaskCreate(TaskFunction_t pxTaskCode, const char* const pcName, const configSTACK_DEPTH_TYPE usStackDepth,
                                     void* const pvParameters, UBaseType_t uxPriority, TaskHandle_t* const pxCreatedTask)
{
    return xTaskCreatePinnedToCore(pxTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pxCreatedTask, tskNO_AFFINITY);
}
static inline TaskHandle_t xTaskCreateStatic(TaskFunction_t pxTaskCode, const char* const pcName, const uint32_t ulStackDepth,
                                             void* const pvParameters, UBaseType_t uxPriority, StackType_t* const puxStackBuffer,
                                             StaticTask_t* const pxTaskBuffer)
{
    return xTaskCreateStaticPinnedToCore(pxTaskCode, pcName, ulStackDepth, pvParameters, uxPriority, puxStackBuffer, pxTaskBuffer,
                                         tskNO_AFFINITY);
}
void vTaskDelete(TaskHandle_t xTaskToDelete);

// Control
void vTaskDelay(const TickType_t xTicksToDelay);
BaseType_t xTaskDelayUntil(TickType_t* const pxPreviousWakeTime, const TickType_t xTimeIncrement);
#define vTaskDelayUntil(pxPreviousWakeTime, xTimeIncrement) do { (void)xTaskDelayUntil((pxPreviousWakeTime), (xTimeIncrement)); } while (0)
BaseType_t xTaskAbortDelay(TaskHandle_t xTask);
UBaseType_t uxTaskPriorityGet(const TaskHandle_t xTask);
UBaseType_t uxTaskPriorityGetFromISR(const TaskHandle_t xTask);
eTaskState eTaskGetState(TaskHandle_t xTask);
void vTaskGetInfo(TaskHandle_t xTask, TaskStatus_t* pxTaskStatus, BaseType_t xGetFreeStackSpace, eTaskState eState);
void vTaskPrioritySet(TaskHandle_t xTask, UBaseType_t uxNewPriority);
void vTaskSuspend(TaskHandle_t xTaskToSuspend);
void vTaskResume(TaskHandle_t xTaskToResume);
BaseType_t xTaskResumeFromISR(TaskHandle_t xTaskToResume);

// Scheduler
void vTaskStartScheduler(void);
void vTaskEndScheduler(void);
void vTaskSuspendAll(void);
BaseType_t xTaskResumeAll(void);
BaseType_t xTaskGetSchedulerState(void);

// Information
TickType_t xTaskGetTickCount(void);
TickType_t xTaskGetTickCountFromISR(void);
UBaseType_t uxTaskGetNumberOfTasks(void);
char* pcTaskGetName(TaskHandle_t xTaskToQuery);
#define pcTaskGetTaskName pcTaskGetName
TaskHandle_t xTaskGetHandle(const char* pcNameToQuery);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t xTask);
configSTACK_DEPTH_TYPE uxTaskGetStackHighWaterMark2(TaskHandle_t xTask);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
TaskHandle_t xTaskGetIdleTaskHandle(void);
UBaseType_t uxTaskGetSystemState(TaskStatus_t* const pxTaskStatusArray, const UBaseType_t uxArraySize, uint32_t* const pulTotalRunTime);
void vTaskList(char* pcWriteBuffer);
void vTaskGetRunTimeStats(char* pcWriteBuffer);
uint32_t ulTaskGetIdleRunTimeCounter(void);
uint32_t ulTaskGetIdleRunTimePercent(void);
void vTaskSetApplicationTaskTag(TaskHandle_t xTask, TaskHookFunction_t pxHookFunction);
TaskHookFunction_t xTaskGetApplicationTaskTag(TaskHandle_t xTask);
BaseType_t xTaskCallApplicationTaskHook(TaskHandle_t xTask, void* pvParameter);
void vTaskSetThreadLocalStoragePointer(TaskHandle_t xTaskToSet, BaseType_t xIndex, void* pvValue);
void* pvTaskGetThreadLocalStoragePointer(TaskHandle_t xTaskToQuery, BaseType_t xIndex);
void vTaskSetThreadLocalStoragePointerAndDelCallback(TaskHandle_t xTaskToSet, BaseType_t xIndex, void* pvValue,
                                                      TlsDeleteCallbackFunction_t pvDelCallback);
void vTaskSetTimeOutState(TimeOut_t* const pxTimeOut);
void vTaskInternalSetTimeOutState(TimeOut_t* const pxTimeOut);
BaseType_t xTaskCheckForTimeOut(TimeOut_t* const pxTimeOut, TickType_t* const pxTicksToWait);
BaseType_t xTaskCatchUpTicks(TickType_t xTicksToCatchUp);
UBaseType_t uxTaskGetTaskNumber(TaskHandle_t xTask);
void vTaskSetTaskNumber(TaskHandle_t xTask, const UBaseType_t uxHandle);
uint8_t* pxTaskGetStackStart(TaskHandle_t xTask);

// ESP-IDF additions
BaseType_t xTaskGetCoreID(TaskHandle_t xTask);
TaskHandle_t xTaskGetIdleTaskHandleForCore(BaseType_t xCoreID);
TaskHandle_t xTaskGetCurrentTaskHandleForCore(BaseType_t xCoreID);
static inline BaseType_t xTaskGetAffinity(TaskHandle_t xTask) { return xTaskGetCoreID(xTask); }
static inline TaskHandle_t xTaskGetIdleTaskHandleForCPU(BaseType_t xCoreID) { return xTaskGetIdleTaskHandleForCore(xCoreID); }
static inline TaskHandle_t xTaskGetCurrentTaskHandleForCPU(BaseType_t xCoreID) { return xTaskGetCurrentTaskHandleForCore(xCoreID); }
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t pvTaskCode, const char* const pcName, const configSTACK_DEPTH_TYPE usStackDepth,
                                           void* const pvParameters, UBaseType_t uxPriority, TaskHandle_t* const pvCreatedTask,
                                           const BaseType_t xCoreID, UBaseType_t uxMemoryCaps);
static inline BaseType_t xTaskCreateWithCaps(TaskFunction_t pvTaskCode, const char* const pcName, const configSTACK_DEPTH_TYPE usStackDepth,
                                             void* const pvParameters, UBaseType_t uxPriority, TaskHandle_t* pvCreatedTask,
                                             UBaseType_t uxMemoryCaps)
{
    return xTaskCreatePinnedToCoreWithCaps(pvTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pvCreatedTask, tskNO_AFFINITY,
                                           uxMemoryCaps);
}
void vTaskDeleteWithCaps(TaskHandle_t xTaskToDelete);

// Notifications
BaseType_t xTaskGenericNotify(TaskHandle_t xTaskToNotify, UBaseType_t uxIndexToNotify, uint32_t ulValue, eNotifyAction eAction,
                              uint32_t* pulPreviousNotificationValue);
BaseType_t xTaskGenericNotifyFromISR(TaskHandle_t xTaskToNotify, UBaseType_t uxIndexToNotify, uint32_t ulValue, eNotifyAction eAction,
                                     uint32_t* pulPreviousNotificationValue, BaseType_t* pxHigherPriorityTaskWoken);
BaseType_t xTaskGenericNotifyWait(UBaseType_t uxIndexToWaitOn, uint32_t ulBitsToClearOnEntry, uint32_t ulBitsToClearOnExit,
                                  uint32_t* pulNotificationValue, TickType_t xTicksToWait);
void vTaskGenericNotifyGiveFromISR(TaskHandle_t xTaskToNotify, UBaseType_t uxIndexToNotify, BaseType_t* pxHigherPriorityTaskWoken);
uint32_t ulTaskGenericNotifyTake(UBaseType_t uxIndexToWaitOn, BaseType_t xClearCountOnExit, TickType_t xTicksToWait);
BaseType_t xTaskGenericNotifyStateClear(TaskHandle_t xTask, UBaseType_t uxIndexToClear);
uint32_t ulTaskGenericNotifyValueClear(TaskHandle_t xTask, UBaseType_t uxIndexToClear, uint32_t ulBitsToClear);

#define xTaskNotify(xTaskToNotify, ulValue, eAction) xTaskGenericNotify((xTaskToNotify), (tskDEFAULT_INDEX_TO_NOTIFY), (ulValue), (eAction), NULL)
#define xTaskNotifyIndexed(xTaskToNotify, uxIndexToNotify, ulValue, eAction) xTaskGenericNotify((xTaskToNotify), (uxIndexToNotify), (ulValue), (eAction), NULL)
#define xTaskNotifyAndQuery(xTaskToNotify, ulValue, eAction, pulPreviousNotifyValue) \
    xTaskGenericNotify((xTaskToNotify), (tskDEFAULT_INDEX_TO_NOTIFY), (ulValue), (eAction), (pulPreviousNotifyValue))
#define xTaskNotifyAndQueryIndexed(xTaskToNotify, uxIndexToNotify, ulValue, eAction, pulPreviousNotifyValue) \
    xTaskGenericNotify((xTaskToNotify), (uxIndexToNotify), (ulValue), (eAction), (pulPreviousNotifyValue))
#define xTaskNotifyFromISR(xTaskToNotify, ulValue, eAction, pxHigherPriorityTaskWoken) \
    xTaskGenericNotifyFromISR((xTaskToNotify), (tskDEFAULT_INDEX_TO_NOTIFY), (ulValue), (eAction), NULL, (pxHigherPriorityTaskWoken))
#define xTaskNotifyIndexedFromISR(xTaskToNotify, uxIndexToNotify, ulValue, eAction, pxHigherPriorityTaskWoken) \
    xTaskGenericNotifyFromISR((xTaskToNotify), (uxIndexToNotify), (ulValue), (eAction), NULL, (pxHigherPriorityTaskWoken))
#define xTaskNotifyAndQueryFromISR(xTaskToNotify, ulValue, eAction, pulPreviousNotificationValue, pxHigherPriorityTaskWoken) \
    xTaskGenericNotifyFromISR((xTaskToNotify), (tskDEFAULT_INDEX_TO_NOTIFY), (ulValue), (eAction), (pulPreviousNotificationValue), (pxHigherPriorityTaskWoken))
#define xTaskNotifyAndQueryIndexedFromISR(xTaskToNotify, uxIndexToNotify, ulValue, eAction, pulPreviousNotificationValue, pxHigherPriorityTaskWoken) \
    xTaskGenericNotifyFromISR((xTaskToNotify), (uxIndexToNotify), (ulValue), (eAction), (pulPreviousNotificationValue), (pxHigherPriorityTaskWoken))
#define xTaskNotifyWait(ulBitsToClearOnEntry, ulBitsToClearOnExit, pulNotificationValue, xTicksToWait) \
    xTaskGenericNotifyWait(tskDEFAULT_INDEX_TO_NOTIFY, (ulBitsToClearOnEntry), (ulBitsToClearOnExit), (pulNotificationValue), (xTicksToWait))
#define xTaskNotifyWaitIndexed(uxIndexToWaitOn, ulBitsToClearOnEntry, ulBitsToClearOnExit, pulNotificationValue, xTicksToWait) \
    xTaskGenericNotifyWait((uxIndexToWaitOn), (ulBitsToClearOnEntry), (ulBitsToClearOnExit), (pulNotificationValue), (xTicksToWait))
#define xTaskNotifyGive(xTaskToNotify) xTaskGenericNotify((xTaskToNotify), (tskDEFAULT_INDEX_TO_NOTIFY), (0), eIncrement, NULL)
#define xTaskNotifyGiveIndexed(xTaskToNotify, uxIndexToNotify) xTaskGenericNotify((xTaskToNotify), (uxIndexToNotify), (0), eIncrement, NULL)
#define vTaskNotifyGiveFromISR(xTaskToNotify, pxHigherPriorityTaskWoken) \
    vTaskGenericNotifyGiveFromISR((xTaskToNotify), (tskDEFAULT_INDEX_TO_NOTIFY), (pxHigherPriorityTaskWoken))
#define vTaskNotifyGiveIndexedFromISR(xTaskToNotify, uxIndexToNotify, pxHigherPriorityTaskWoken) \
    vTaskGenericNotifyGiveFromISR((xTaskToNotify), (uxIndexToNotify), (pxHigherPriorityTaskWoken))
#define ulTaskNotifyTake(xClearCountOnExit, xTicksToWait) ulTaskGenericNotifyTake((tskDEFAULT_INDEX_TO_NOTIFY), (xClearCountOnExit), (xTicksToWait))
#define ulTaskNotifyTakeIndexed(uxIndexToWaitOn, xClearCountOnExit, xTicksToWait) ulTaskGenericNotifyTake((uxIndexToWaitOn), (xClearCountOnExit), (xTicksToWait))
#define xTaskNotifyStateClear(xTask) xTaskGenericNotifyStateClear((xTask), (tskDEFAULT_INDEX_TO_NOTIFY))
#define xTaskNotifyStateClearIndexed(xTask, uxIndexToClear) xTaskGenericNotifyStateClear((xTask), (uxIndexToClear))
#define ulTaskNotifyValueClear(xTask, ulBitsToClear) ulTaskGenericNotifyValueClear((xTask), (tskDEFAULT_INDEX_TO_NOTIFY), (ulBitsToClear))
#define ulTaskNotifyValueClearIndexed(xTask, uxIndexToClear, ulBitsToClear) ulTaskGenericNotifyValueClear((xTask), (uxIndexToClear), (ulBitsToClear))

// Heap (FreeRTOS portable.h)
size_t xPortGetFreeHeapSize(void);
size_t xPortGetMinimumEverFreeHeapSize(void);
void* pvPortMalloc(size_t xWantedSize);
void vPortFree(void* pv);

CARDENZA_WEB_EXTERN_C_END
