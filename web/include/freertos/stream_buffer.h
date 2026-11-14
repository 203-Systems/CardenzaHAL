#pragma once

#include "freertos/task.h"

CARDENZA_WEB_EXTERN_C_BEGIN

typedef void (*StreamBufferCallbackFunction_t)(StreamBufferHandle_t xStreamBuffer, BaseType_t xIsInsideISR,
                                               BaseType_t* const pxHigherPriorityTaskWoken);

StreamBufferHandle_t xStreamBufferGenericCreate(size_t xBufferSizeBytes, size_t xTriggerLevelBytes, BaseType_t xIsMessageBuffer,
                                                StreamBufferCallbackFunction_t pxSendCompletedCallback,
                                                StreamBufferCallbackFunction_t pxReceiveCompletedCallback);
StreamBufferHandle_t xStreamBufferGenericCreateStatic(size_t xBufferSizeBytes, size_t xTriggerLevelBytes, BaseType_t xIsMessageBuffer,
                                                      uint8_t* const pucStreamBufferStorageArea,
                                                      StaticStreamBuffer_t* const pxStaticStreamBuffer,
                                                      StreamBufferCallbackFunction_t pxSendCompletedCallback,
                                                      StreamBufferCallbackFunction_t pxReceiveCompletedCallback);
size_t xStreamBufferSend(StreamBufferHandle_t xStreamBuffer, const void* pvTxData, size_t xDataLengthBytes, TickType_t xTicksToWait);
size_t xStreamBufferSendFromISR(StreamBufferHandle_t xStreamBuffer, const void* pvTxData, size_t xDataLengthBytes,
                                BaseType_t* const pxHigherPriorityTaskWoken);
size_t xStreamBufferReceive(StreamBufferHandle_t xStreamBuffer, void* pvRxData, size_t xBufferLengthBytes, TickType_t xTicksToWait);
size_t xStreamBufferReceiveFromISR(StreamBufferHandle_t xStreamBuffer, void* pvRxData, size_t xBufferLengthBytes,
                                   BaseType_t* const pxHigherPriorityTaskWoken);
void vStreamBufferDelete(StreamBufferHandle_t xStreamBuffer);
BaseType_t xStreamBufferIsFull(StreamBufferHandle_t xStreamBuffer);
BaseType_t xStreamBufferIsEmpty(StreamBufferHandle_t xStreamBuffer);
BaseType_t xStreamBufferReset(StreamBufferHandle_t xStreamBuffer);
size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t xStreamBuffer);
size_t xStreamBufferBytesAvailable(StreamBufferHandle_t xStreamBuffer);
BaseType_t xStreamBufferSetTriggerLevel(StreamBufferHandle_t xStreamBuffer, size_t xTriggerLevel);
size_t xStreamBufferNextMessageLengthBytes(StreamBufferHandle_t xStreamBuffer);

#define xStreamBufferCreate(xBufferSizeBytes, xTriggerLevelBytes) xStreamBufferGenericCreate((xBufferSizeBytes), (xTriggerLevelBytes), pdFALSE, NULL, NULL)
#define xStreamBufferCreateStatic(xBufferSizeBytes, xTriggerLevelBytes, pucStreamBufferStorageArea, pxStaticStreamBuffer) \
    xStreamBufferGenericCreateStatic((xBufferSizeBytes), (xTriggerLevelBytes), pdFALSE, (pucStreamBufferStorageArea), (pxStaticStreamBuffer), NULL, NULL)
#define xStreamBufferCreateWithCaps(xBufferSizeBytes, xTriggerLevelBytes, caps) xStreamBufferCreate((xBufferSizeBytes), (xTriggerLevelBytes))
#define vStreamBufferDeleteWithCaps(xStreamBuffer) vStreamBufferDelete((xStreamBuffer))

CARDENZA_WEB_EXTERN_C_END
