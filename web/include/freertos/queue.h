#pragma once

#include "freertos/FreeRTOS.h"

CARDENZA_WEB_EXTERN_C_BEGIN

#define queueSEND_TO_BACK ((BaseType_t)0)
#define queueSEND_TO_FRONT ((BaseType_t)1)
#define queueOVERWRITE ((BaseType_t)2)
#define queueQUEUE_TYPE_BASE ((uint8_t)0U)
#define queueQUEUE_TYPE_SET ((uint8_t)0U)
#define queueQUEUE_TYPE_MUTEX ((uint8_t)1U)
#define queueQUEUE_TYPE_COUNTING_SEMAPHORE ((uint8_t)2U)
#define queueQUEUE_TYPE_BINARY_SEMAPHORE ((uint8_t)3U)
#define queueQUEUE_TYPE_RECURSIVE_MUTEX ((uint8_t)4U)

QueueHandle_t xQueueGenericCreate(const UBaseType_t uxQueueLength, const UBaseType_t uxItemSize, const uint8_t ucQueueType);
QueueHandle_t xQueueGenericCreateStatic(const UBaseType_t uxQueueLength, const UBaseType_t uxItemSize, uint8_t* pucQueueStorage,
                                        StaticQueue_t* pxStaticQueue, const uint8_t ucQueueType);
BaseType_t xQueueGenericGetStaticBuffers(QueueHandle_t xQueue, uint8_t** ppucQueueStorage, StaticQueue_t** ppxStaticQueue);
BaseType_t xQueueGenericSend(QueueHandle_t xQueue, const void* const pvItemToQueue, TickType_t xTicksToWait, const BaseType_t xCopyPosition);
BaseType_t xQueueGenericSendFromISR(QueueHandle_t xQueue, const void* const pvItemToQueue, BaseType_t* const pxHigherPriorityTaskWoken,
                                    const BaseType_t xCopyPosition);
BaseType_t xQueueGiveFromISR(QueueHandle_t xQueue, BaseType_t* const pxHigherPriorityTaskWoken);
BaseType_t xQueueReceive(QueueHandle_t xQueue, void* const pvBuffer, TickType_t xTicksToWait);
BaseType_t xQueuePeek(QueueHandle_t xQueue, void* const pvBuffer, TickType_t xTicksToWait);
BaseType_t xQueuePeekFromISR(QueueHandle_t xQueue, void* const pvBuffer);
BaseType_t xQueueReceiveFromISR(QueueHandle_t xQueue, void* const pvBuffer, BaseType_t* const pxHigherPriorityTaskWoken);
BaseType_t xQueueSemaphoreTake(QueueHandle_t xQueue, TickType_t xTicksToWait);
UBaseType_t uxQueueMessagesWaiting(const QueueHandle_t xQueue);
UBaseType_t uxQueueMessagesWaitingFromISR(const QueueHandle_t xQueue);
UBaseType_t uxQueueSpacesAvailable(const QueueHandle_t xQueue);
void vQueueDelete(QueueHandle_t xQueue);
BaseType_t xQueueGenericReset(QueueHandle_t xQueue, BaseType_t xNewQueue);
BaseType_t xQueueIsQueueEmptyFromISR(const QueueHandle_t xQueue);
BaseType_t xQueueIsQueueFullFromISR(const QueueHandle_t xQueue);
QueueHandle_t xQueueCreateMutex(const uint8_t ucQueueType);
QueueHandle_t xQueueCreateMutexStatic(const uint8_t ucQueueType, StaticQueue_t* pxStaticQueue);
QueueHandle_t xQueueCreateCountingSemaphore(const UBaseType_t uxMaxCount, const UBaseType_t uxInitialCount);
QueueHandle_t xQueueCreateCountingSemaphoreStatic(const UBaseType_t uxMaxCount, const UBaseType_t uxInitialCount, StaticQueue_t* pxStaticQueue);
TaskHandle_t xQueueGetMutexHolder(QueueHandle_t xSemaphore);
TaskHandle_t xQueueGetMutexHolderFromISR(QueueHandle_t xSemaphore);
BaseType_t xQueueTakeMutexRecursive(QueueHandle_t xMutex, TickType_t xTicksToWait);
BaseType_t xQueueGiveMutexRecursive(QueueHandle_t xMutex);
void vQueueAddToRegistry(QueueHandle_t xQueue, const char* pcQueueName);
void vQueueUnregisterQueue(QueueHandle_t xQueue);
const char* pcQueueGetName(QueueHandle_t xQueue);
QueueSetHandle_t xQueueCreateSet(const UBaseType_t uxEventQueueLength);
BaseType_t xQueueAddToSet(QueueSetMemberHandle_t xQueueOrSemaphore, QueueSetHandle_t xQueueSet);
BaseType_t xQueueRemoveFromSet(QueueSetMemberHandle_t xQueueOrSemaphore, QueueSetHandle_t xQueueSet);
QueueSetMemberHandle_t xQueueSelectFromSet(QueueSetHandle_t xQueueSet, const TickType_t xTicksToWait);
QueueSetMemberHandle_t xQueueSelectFromSetFromISR(QueueSetHandle_t xQueueSet);
QueueHandle_t xQueueCreateWithCaps(UBaseType_t uxQueueLength, UBaseType_t uxItemSize, UBaseType_t uxMemoryCaps);
void vQueueDeleteWithCaps(QueueHandle_t xQueue);

#define xQueueCreate(uxQueueLength, uxItemSize) xQueueGenericCreate((uxQueueLength), (uxItemSize), (queueQUEUE_TYPE_BASE))
#define xQueueCreateStatic(uxQueueLength, uxItemSize, pucQueueStorage, pxQueueBuffer) \
    xQueueGenericCreateStatic((uxQueueLength), (uxItemSize), (pucQueueStorage), (pxQueueBuffer), (queueQUEUE_TYPE_BASE))
#define xQueueGetStaticBuffers(xQueue, ppucQueueStorage, ppxStaticQueue) xQueueGenericGetStaticBuffers((xQueue), (ppucQueueStorage), (ppxStaticQueue))
#define xQueueSendToFront(xQueue, pvItemToQueue, xTicksToWait) xQueueGenericSend((xQueue), (pvItemToQueue), (xTicksToWait), queueSEND_TO_FRONT)
#define xQueueSendToBack(xQueue, pvItemToQueue, xTicksToWait) xQueueGenericSend((xQueue), (pvItemToQueue), (xTicksToWait), queueSEND_TO_BACK)
#define xQueueSend(xQueue, pvItemToQueue, xTicksToWait) xQueueGenericSend((xQueue), (pvItemToQueue), (xTicksToWait), queueSEND_TO_BACK)
#define xQueueOverwrite(xQueue, pvItemToQueue) xQueueGenericSend((xQueue), (pvItemToQueue), 0, queueOVERWRITE)
#define xQueueSendToFrontFromISR(xQueue, pvItemToQueue, pxHigherPriorityTaskWoken) \
    xQueueGenericSendFromISR((xQueue), (pvItemToQueue), (pxHigherPriorityTaskWoken), queueSEND_TO_FRONT)
#define xQueueSendToBackFromISR(xQueue, pvItemToQueue, pxHigherPriorityTaskWoken) \
    xQueueGenericSendFromISR((xQueue), (pvItemToQueue), (pxHigherPriorityTaskWoken), queueSEND_TO_BACK)
#define xQueueOverwriteFromISR(xQueue, pvItemToQueue, pxHigherPriorityTaskWoken) \
    xQueueGenericSendFromISR((xQueue), (pvItemToQueue), (pxHigherPriorityTaskWoken), queueOVERWRITE)
#define xQueueSendFromISR(xQueue, pvItemToQueue, pxHigherPriorityTaskWoken) \
    xQueueGenericSendFromISR((xQueue), (pvItemToQueue), (pxHigherPriorityTaskWoken), queueSEND_TO_BACK)
#define xQueueReset(xQueue) xQueueGenericReset((xQueue), pdFALSE)

CARDENZA_WEB_EXTERN_C_END
