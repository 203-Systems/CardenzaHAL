#pragma once

#include "freertos/stream_buffer.h"

#define xMessageBufferCreate(xBufferSizeBytes) xStreamBufferGenericCreate((xBufferSizeBytes), (size_t)0, pdTRUE, NULL, NULL)
#define xMessageBufferCreateStatic(xBufferSizeBytes, pucMessageBufferStorageArea, pxStaticMessageBuffer) \
    xStreamBufferGenericCreateStatic((xBufferSizeBytes), 0, pdTRUE, (pucMessageBufferStorageArea), (pxStaticMessageBuffer), NULL, NULL)
#define xMessageBufferSend(xMessageBuffer, pvTxData, xDataLengthBytes, xTicksToWait) \
    xStreamBufferSend((xMessageBuffer), (pvTxData), (xDataLengthBytes), (xTicksToWait))
#define xMessageBufferSendFromISR(xMessageBuffer, pvTxData, xDataLengthBytes, pxHigherPriorityTaskWoken) \
    xStreamBufferSendFromISR((xMessageBuffer), (pvTxData), (xDataLengthBytes), (pxHigherPriorityTaskWoken))
#define xMessageBufferReceive(xMessageBuffer, pvRxData, xBufferLengthBytes, xTicksToWait) \
    xStreamBufferReceive((xMessageBuffer), (pvRxData), (xBufferLengthBytes), (xTicksToWait))
#define xMessageBufferReceiveFromISR(xMessageBuffer, pvRxData, xBufferLengthBytes, pxHigherPriorityTaskWoken) \
    xStreamBufferReceiveFromISR((xMessageBuffer), (pvRxData), (xBufferLengthBytes), (pxHigherPriorityTaskWoken))
#define vMessageBufferDelete(xMessageBuffer) vStreamBufferDelete((xMessageBuffer))
#define xMessageBufferIsFull(xMessageBuffer) xStreamBufferIsFull((xMessageBuffer))
#define xMessageBufferIsEmpty(xMessageBuffer) xStreamBufferIsEmpty((xMessageBuffer))
#define xMessageBufferReset(xMessageBuffer) xStreamBufferReset((xMessageBuffer))
#define xMessageBufferSpaceAvailable(xMessageBuffer) xStreamBufferSpacesAvailable((xMessageBuffer))
#define xMessageBufferSpacesAvailable(xMessageBuffer) xStreamBufferSpacesAvailable((xMessageBuffer))
#define xMessageBufferNextLengthBytes(xMessageBuffer) xStreamBufferNextMessageLengthBytes((xMessageBuffer))
