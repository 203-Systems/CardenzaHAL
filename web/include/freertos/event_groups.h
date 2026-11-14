#pragma once

#include "freertos/timers.h"

CARDENZA_WEB_EXTERN_C_BEGIN

EventGroupHandle_t xEventGroupCreate(void);
EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t* pxEventGroupBuffer);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t xEventGroup, const EventBits_t uxBitsToWaitFor, const BaseType_t xClearOnExit,
                                const BaseType_t xWaitForAllBits, TickType_t xTicksToWait);
EventBits_t xEventGroupClearBits(EventGroupHandle_t xEventGroup, const EventBits_t uxBitsToClear);
BaseType_t xEventGroupClearBitsFromISR(EventGroupHandle_t xEventGroup, const EventBits_t uxBitsToClear);
EventBits_t xEventGroupSetBits(EventGroupHandle_t xEventGroup, const EventBits_t uxBitsToSet);
BaseType_t xEventGroupSetBitsFromISR(EventGroupHandle_t xEventGroup, const EventBits_t uxBitsToSet, BaseType_t* pxHigherPriorityTaskWoken);
EventBits_t xEventGroupSync(EventGroupHandle_t xEventGroup, const EventBits_t uxBitsToSet, const EventBits_t uxBitsToWaitFor,
                            TickType_t xTicksToWait);
EventBits_t xEventGroupGetBitsFromISR(EventGroupHandle_t xEventGroup);
void vEventGroupDelete(EventGroupHandle_t xEventGroup);
BaseType_t xEventGroupGetStaticBuffer(EventGroupHandle_t xEventGroup, StaticEventGroup_t** ppxEventGroupBuffer);
#define xEventGroupGetBits(xEventGroup) xEventGroupClearBits((xEventGroup), 0)
#define xEventGroupCreateWithCaps(caps) xEventGroupCreate()
#define vEventGroupDeleteWithCaps(xEventGroup) vEventGroupDelete((xEventGroup))

CARDENZA_WEB_EXTERN_C_END
