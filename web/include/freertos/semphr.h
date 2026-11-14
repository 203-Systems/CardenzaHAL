#pragma once

#include "freertos/queue.h"

typedef QueueHandle_t SemaphoreHandle_t;

#define semBINARY_SEMAPHORE_QUEUE_LENGTH ((uint8_t)1U)
#define semSEMAPHORE_QUEUE_ITEM_LENGTH ((uint8_t)0U)
#define semGIVE_BLOCK_TIME ((TickType_t)0U)

#define vSemaphoreCreateBinary(xSemaphore) do { \
        (xSemaphore) = xQueueGenericCreate((UBaseType_t)1, semSEMAPHORE_QUEUE_ITEM_LENGTH, queueQUEUE_TYPE_BINARY_SEMAPHORE); \
        if ((xSemaphore) != NULL) (void)xSemaphoreGive((xSemaphore)); } while (0)
#define xSemaphoreCreateBinary() xQueueGenericCreate((UBaseType_t)1, semSEMAPHORE_QUEUE_ITEM_LENGTH, queueQUEUE_TYPE_BINARY_SEMAPHORE)
#define xSemaphoreCreateBinaryStatic(pxStaticSemaphore) \
    xQueueGenericCreateStatic((UBaseType_t)1, semSEMAPHORE_QUEUE_ITEM_LENGTH, NULL, (pxStaticSemaphore), queueQUEUE_TYPE_BINARY_SEMAPHORE)
#define xSemaphoreTake(xSemaphore, xBlockTime) xQueueSemaphoreTake((xSemaphore), (xBlockTime))
#define xSemaphoreTakeRecursive(xMutex, xBlockTime) xQueueTakeMutexRecursive((xMutex), (xBlockTime))
#define xSemaphoreGive(xSemaphore) xQueueGenericSend((QueueHandle_t)(xSemaphore), NULL, semGIVE_BLOCK_TIME, queueSEND_TO_BACK)
#define xSemaphoreGiveRecursive(xMutex) xQueueGiveMutexRecursive((xMutex))
#define xSemaphoreGiveFromISR(xSemaphore, pxHigherPriorityTaskWoken) xQueueGiveFromISR((QueueHandle_t)(xSemaphore), (pxHigherPriorityTaskWoken))
#define xSemaphoreTakeFromISR(xSemaphore, pxHigherPriorityTaskWoken) xQueueReceiveFromISR((QueueHandle_t)(xSemaphore), NULL, (pxHigherPriorityTaskWoken))
#define xSemaphoreCreateMutex() xQueueCreateMutex(queueQUEUE_TYPE_MUTEX)
#define xSemaphoreCreateMutexStatic(pxMutexBuffer) xQueueCreateMutexStatic(queueQUEUE_TYPE_MUTEX, (pxMutexBuffer))
#define xSemaphoreCreateRecursiveMutex() xQueueCreateMutex(queueQUEUE_TYPE_RECURSIVE_MUTEX)
#define xSemaphoreCreateRecursiveMutexStatic(pxStaticSemaphore) xQueueCreateMutexStatic(queueQUEUE_TYPE_RECURSIVE_MUTEX, (pxStaticSemaphore))
#define xSemaphoreCreateCounting(uxMaxCount, uxInitialCount) xQueueCreateCountingSemaphore((uxMaxCount), (uxInitialCount))
#define xSemaphoreCreateCountingStatic(uxMaxCount, uxInitialCount, pxSemaphoreBuffer) \
    xQueueCreateCountingSemaphoreStatic((uxMaxCount), (uxInitialCount), (pxSemaphoreBuffer))
#define xSemaphoreGetStaticBuffer(xSemaphore, ppxSemaphoreBuffer) xQueueGenericGetStaticBuffers((QueueHandle_t)(xSemaphore), NULL, (ppxSemaphoreBuffer))
#define vSemaphoreDelete(xSemaphore) vQueueDelete((QueueHandle_t)(xSemaphore))
#define xSemaphoreGetMutexHolder(xSemaphore) xQueueGetMutexHolder((xSemaphore))
#define xSemaphoreGetMutexHolderFromISR(xSemaphore) xQueueGetMutexHolderFromISR((xSemaphore))
#define uxSemaphoreGetCount(xSemaphore) uxQueueMessagesWaiting((QueueHandle_t)(xSemaphore))
#define uxSemaphoreGetCountFromISR(xSemaphore) uxQueueMessagesWaitingFromISR((QueueHandle_t)(xSemaphore))
#define xSemaphoreCreateBinaryWithCaps(caps) xSemaphoreCreateBinary()
#define xSemaphoreCreateCountingWithCaps(max, initial, caps) xSemaphoreCreateCounting((max), (initial))
#define xSemaphoreCreateMutexWithCaps(caps) xSemaphoreCreateMutex()
#define xSemaphoreCreateRecursiveMutexWithCaps(caps) xSemaphoreCreateRecursiveMutex()
#define vSemaphoreDeleteWithCaps(xSemaphore) vSemaphoreDelete((xSemaphore))
