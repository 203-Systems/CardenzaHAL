// FreeRTOS event groups, software timers (the "Tmr Svc" daemon task) and stream / message buffers.

#include <cstdlib>
#include <cstring>
#include <new>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "kernel.h"

namespace k = cardenza_web::kernel;

namespace cardenza_web::freertos {
void EnsureStarted();
}

// ---- event groups ---------------------------------------------------------------------------------

struct EventGroupDef_t {
    uint32_t magic;
    EventBits_t bits;
    k::WaitQueue waiters;
    bool ownsObject;
    StaticEventGroup_t* staticBuffer;
};
static_assert(sizeof(EventGroupDef_t) <= sizeof(StaticEventGroup_t), "StaticEventGroup_t must hold an event group");

namespace {

constexpr uint32_t kEventMagic = 0x45564E54;
constexpr EventBits_t kControlBits = 0xFF000000u;  // FreeRTOS keeps the top byte for itself

// What a waiter asked for, kept with it while it waits.
struct EventWait {
    EventBits_t want;
    bool all;
    bool clear;
};

bool ValidGroup(const EventGroupDef_t* group) { return group != nullptr && group->magic == kEventMagic; }

bool Satisfied(EventBits_t bits, const EventWait& wait)
{
    return wait.all ? (bits & wait.want) == wait.want : (bits & wait.want) != 0;
}

// Wakes every waiter the bits satisfy; each gets the bits as they were, then the bits asked to be
// cleared on exit are cleared, as FreeRTOS does.
void Evaluate(EventGroupDef_t* group, int* woken)
{
    EventBits_t clear = 0;
    k::Task* task = group->waiters.head;
    while (task != nullptr)
    {
        k::Task* next = task->waitNext;
        const auto* wait = static_cast<const EventWait*>(task->waitData);
        if (wait != nullptr && Satisfied(group->bits, *wait))
        {
            if (wait->clear) clear |= wait->want;
            k::WakeTask(task, group->bits | 0x80000000u, woken);  // the top bit: "satisfied"
        }
        task = next;
    }
    group->bits &= ~clear;
}

EventBits_t Wait(EventGroupDef_t* group, EventBits_t want, bool clear, bool all, TickType_t ticks)
{
    EventWait wait{want & ~kControlBits, all, clear};
    if (Satisfied(group->bits, wait))
    {
        const EventBits_t bits = group->bits;
        if (clear) group->bits &= ~wait.want;
        k::PreemptionPoint();
        return bits;
    }
    if (ticks == 0 || !k::InTask()) return group->bits;
    k::Task* task = k::Current();
    const uint64_t deadline = k::DeadlineForTicks(ticks);
    task->waitData = &wait;
    const bool woken = k::Block(&group->waiters, deadline);
    task->waitData = nullptr;
    if (woken && (task->wakeValue & 0x80000000u) != 0) return static_cast<EventBits_t>(task->wakeValue & ~0x80000000u) & ~kControlBits;
    return group->bits;
}

}  // namespace

extern "C" {

EventGroupHandle_t xEventGroupCreate(void)
{
    cardenza_web::freertos::EnsureStarted();
    void* memory = std::malloc(sizeof(EventGroupDef_t));
    if (memory == nullptr) return nullptr;
    auto* group = new (memory) EventGroupDef_t{kEventMagic, 0, {}, true, nullptr};
    return group;
}

EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t* pxEventGroupBuffer)
{
    if (pxEventGroupBuffer == nullptr) return nullptr;
    return new (pxEventGroupBuffer) EventGroupDef_t{kEventMagic, 0, {}, false, pxEventGroupBuffer};
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t xEventGroup, const EventBits_t uxBitsToWaitFor, const BaseType_t xClearOnExit,
                                const BaseType_t xWaitForAllBits, TickType_t xTicksToWait)
{
    if (!ValidGroup(xEventGroup)) return 0;
    return Wait(xEventGroup, uxBitsToWaitFor, xClearOnExit != pdFALSE, xWaitForAllBits != pdFALSE, xTicksToWait);
}

EventBits_t xEventGroupClearBits(EventGroupHandle_t xEventGroup, const EventBits_t uxBitsToClear)
{
    if (!ValidGroup(xEventGroup)) return 0;
    const EventBits_t bits = xEventGroup->bits;
    xEventGroup->bits &= ~(uxBitsToClear & ~kControlBits);
    return bits;
}

BaseType_t xEventGroupClearBitsFromISR(EventGroupHandle_t xEventGroup, const EventBits_t uxBitsToClear)
{
    xEventGroupClearBits(xEventGroup, uxBitsToClear);
    return pdPASS;
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t xEventGroup, const EventBits_t uxBitsToSet)
{
    if (!ValidGroup(xEventGroup)) return 0;
    xEventGroup->bits |= uxBitsToSet & ~kControlBits;
    Evaluate(xEventGroup, nullptr);
    const EventBits_t bits = xEventGroup->bits;
    k::PreemptionPoint();
    return bits;
}

BaseType_t xEventGroupSetBitsFromISR(EventGroupHandle_t xEventGroup, const EventBits_t uxBitsToSet, BaseType_t* pxHigherPriorityTaskWoken)
{
    if (!ValidGroup(xEventGroup)) return pdFAIL;
    int woken = 0;
    xEventGroup->bits |= uxBitsToSet & ~kControlBits;
    Evaluate(xEventGroup, &woken);
    if (woken && pxHigherPriorityTaskWoken != nullptr) *pxHigherPriorityTaskWoken = pdTRUE;
    k::YieldFromIsrRequest(woken);
    return pdPASS;
}

EventBits_t xEventGroupSync(EventGroupHandle_t xEventGroup, const EventBits_t uxBitsToSet, const EventBits_t uxBitsToWaitFor,
                            TickType_t xTicksToWait)
{
    if (!ValidGroup(xEventGroup)) return 0;
    xEventGroup->bits |= uxBitsToSet & ~kControlBits;
    Evaluate(xEventGroup, nullptr);
    return Wait(xEventGroup, uxBitsToWaitFor, true, true, xTicksToWait);
}

EventBits_t xEventGroupGetBitsFromISR(EventGroupHandle_t xEventGroup)
{
    return ValidGroup(xEventGroup) ? xEventGroup->bits : 0;
}

void vEventGroupDelete(EventGroupHandle_t xEventGroup)
{
    if (!ValidGroup(xEventGroup)) return;
    while (xEventGroup->waiters.head != nullptr) k::WakeTask(xEventGroup->waiters.head, 0, nullptr);
    xEventGroup->magic = 0;
    if (xEventGroup->ownsObject) std::free(xEventGroup);
}

BaseType_t xEventGroupGetStaticBuffer(EventGroupHandle_t xEventGroup, StaticEventGroup_t** ppxEventGroupBuffer)
{
    if (!ValidGroup(xEventGroup) || xEventGroup->staticBuffer == nullptr) return pdFALSE;
    *ppxEventGroupBuffer = xEventGroup->staticBuffer;
    return pdTRUE;
}

}  // extern "C"

// ---- software timers ------------------------------------------------------------------------------

struct tmrTimerControl {
    uint32_t magic;
    const char* name;
    TickType_t period;
    bool autoReload;
    bool active;
    void* id;
    TimerCallbackFunction_t callback;
    uint64_t expiryUs;
    tmrTimerControl* next;
    bool ownsObject;
    StaticTimer_t* staticBuffer;
};
static_assert(sizeof(tmrTimerControl) <= sizeof(StaticTimer_t), "StaticTimer_t must hold a timer");

namespace {

constexpr uint32_t kTimerMagic = 0x544D5253;

struct PendedCall {
    PendedFunction_t function;
    void* parameter1;
    uint32_t parameter2;
};

tmrTimerControl* g_timers = nullptr;
k::Task* g_timerTask = nullptr;
k::WaitQueue g_timerWake;
constexpr int kPendedCalls = 16;
PendedCall g_pended[kPendedCalls];
int g_pendedHead = 0;
int g_pendedCount = 0;

bool ValidTimer(const tmrTimerControl* timer) { return timer != nullptr && timer->magic == kTimerMagic; }

void TimerTask(void*)
{
    for (;;)
    {
        while (g_pendedCount > 0)
        {
            const PendedCall call = g_pended[g_pendedHead];
            g_pendedHead = (g_pendedHead + 1) % kPendedCalls;
            --g_pendedCount;
            call.function(call.parameter1, call.parameter2);
        }
        uint64_t next = k::kForever;
        const uint64_t now = k::NowUs();
        for (tmrTimerControl* timer = g_timers; timer != nullptr; timer = timer->next)
        {
            if (!timer->active) continue;
            if (timer->expiryUs <= now)
            {
                if (timer->autoReload)
                {
                    timer->expiryUs += static_cast<uint64_t>(timer->period) * k::TickUs();
                    if (timer->expiryUs <= now) timer->expiryUs = now + static_cast<uint64_t>(timer->period) * k::TickUs();
                }
                else
                {
                    timer->active = false;
                }
                timer->callback(timer);
                next = 0;  // the callback may have changed the list: look again
                break;
            }
            if (timer->expiryUs < next) next = timer->expiryUs;
        }
        if (next == 0 || g_pendedCount > 0) continue;
        k::Block(&g_timerWake, next);
    }
}

void EnsureTimerTask()
{
    if (g_timerTask != nullptr) return;
    cardenza_web::freertos::EnsureStarted();
    g_timerTask = k::CreateTask(TimerTask, nullptr, "Tmr Svc", configTIMER_TASK_STACK_DEPTH, configTIMER_TASK_PRIORITY, 0, true);
}

void Kick(int* woken)
{
    EnsureTimerTask();
    k::WakeAll(&g_timerWake, 0, woken);
}

void Start(tmrTimerControl* timer, int* woken)
{
    timer->expiryUs = k::NowUs() + static_cast<uint64_t>(timer->period) * k::TickUs();
    timer->active = true;
    Kick(woken);
}

tmrTimerControl* CreateTimer(void* memory, bool owns, const char* name, TickType_t period, BaseType_t autoReload, void* id,
                             TimerCallbackFunction_t callback, StaticTimer_t* buffer)
{
    auto* timer = new (memory) tmrTimerControl{};
    timer->magic = kTimerMagic;
    timer->name = name;
    timer->period = period;
    timer->autoReload = autoReload != pdFALSE;
    timer->id = id;
    timer->callback = callback;
    timer->ownsObject = owns;
    timer->staticBuffer = buffer;
    timer->next = g_timers;
    g_timers = timer;
    EnsureTimerTask();
    return timer;
}

}  // namespace

extern "C" {

TimerHandle_t xTimerCreate(const char* const pcTimerName, const TickType_t xTimerPeriodInTicks, const BaseType_t xAutoReload,
                           void* const pvTimerID, TimerCallbackFunction_t pxCallbackFunction)
{
    if (xTimerPeriodInTicks == 0 || pxCallbackFunction == nullptr) return nullptr;
    void* memory = std::malloc(sizeof(tmrTimerControl));
    if (memory == nullptr) return nullptr;
    return CreateTimer(memory, true, pcTimerName, xTimerPeriodInTicks, xAutoReload, pvTimerID, pxCallbackFunction, nullptr);
}

TimerHandle_t xTimerCreateStatic(const char* const pcTimerName, const TickType_t xTimerPeriodInTicks, const BaseType_t xAutoReload,
                                 void* const pvTimerID, TimerCallbackFunction_t pxCallbackFunction, StaticTimer_t* pxTimerBuffer)
{
    if (xTimerPeriodInTicks == 0 || pxCallbackFunction == nullptr || pxTimerBuffer == nullptr) return nullptr;
    return CreateTimer(pxTimerBuffer, false, pcTimerName, xTimerPeriodInTicks, xAutoReload, pvTimerID, pxCallbackFunction, pxTimerBuffer);
}

void* pvTimerGetTimerID(const TimerHandle_t xTimer) { return ValidTimer(xTimer) ? xTimer->id : nullptr; }

void vTimerSetTimerID(TimerHandle_t xTimer, void* pvNewID)
{
    if (ValidTimer(xTimer)) xTimer->id = pvNewID;
}

BaseType_t xTimerIsTimerActive(TimerHandle_t xTimer) { return ValidTimer(xTimer) && xTimer->active ? pdTRUE : pdFALSE; }

TaskHandle_t xTimerGetTimerDaemonTaskHandle(void)
{
    EnsureTimerTask();
    return reinterpret_cast<TaskHandle_t>(g_timerTask);
}

BaseType_t xTimerPendFunctionCallFromISR(PendedFunction_t xFunctionToPend, void* pvParameter1, uint32_t ulParameter2,
                                         BaseType_t* pxHigherPriorityTaskWoken)
{
    if (g_pendedCount >= kPendedCalls) return pdFAIL;
    g_pended[(g_pendedHead + g_pendedCount) % kPendedCalls] = {xFunctionToPend, pvParameter1, ulParameter2};
    ++g_pendedCount;
    int woken = 0;
    Kick(&woken);
    if (woken && pxHigherPriorityTaskWoken != nullptr) *pxHigherPriorityTaskWoken = pdTRUE;
    return pdPASS;
}

BaseType_t xTimerPendFunctionCall(PendedFunction_t xFunctionToPend, void* pvParameter1, uint32_t ulParameter2, TickType_t)
{
    const BaseType_t result = xTimerPendFunctionCallFromISR(xFunctionToPend, pvParameter1, ulParameter2, nullptr);
    k::PreemptionPoint();
    return result;
}

const char* pcTimerGetName(TimerHandle_t xTimer) { return ValidTimer(xTimer) ? xTimer->name : nullptr; }

void vTimerSetReloadMode(TimerHandle_t xTimer, const BaseType_t xAutoReload)
{
    if (ValidTimer(xTimer)) xTimer->autoReload = xAutoReload != pdFALSE;
}

BaseType_t xTimerGetReloadMode(TimerHandle_t xTimer) { return ValidTimer(xTimer) && xTimer->autoReload ? pdTRUE : pdFALSE; }
UBaseType_t uxTimerGetReloadMode(TimerHandle_t xTimer) { return xTimerGetReloadMode(xTimer); }
TickType_t xTimerGetPeriod(TimerHandle_t xTimer) { return ValidTimer(xTimer) ? xTimer->period : 0; }

TickType_t xTimerGetExpiryTime(TimerHandle_t xTimer)
{
    return ValidTimer(xTimer) ? static_cast<TickType_t>(xTimer->expiryUs / k::TickUs()) : 0;
}

BaseType_t xTimerGetStaticBuffer(TimerHandle_t xTimer, StaticTimer_t** ppxTimerBuffer)
{
    if (!ValidTimer(xTimer) || xTimer->staticBuffer == nullptr) return pdFALSE;
    *ppxTimerBuffer = xTimer->staticBuffer;
    return pdTRUE;
}

BaseType_t xTimerStart(TimerHandle_t xTimer, TickType_t)
{
    if (!ValidTimer(xTimer)) return pdFAIL;
    Start(xTimer, nullptr);
    k::PreemptionPoint();
    return pdPASS;
}

BaseType_t xTimerReset(TimerHandle_t xTimer, TickType_t xTicksToWait) { return xTimerStart(xTimer, xTicksToWait); }

BaseType_t xTimerStop(TimerHandle_t xTimer, TickType_t)
{
    if (!ValidTimer(xTimer)) return pdFAIL;
    xTimer->active = false;
    k::PreemptionPoint();
    return pdPASS;
}

BaseType_t xTimerChangePeriod(TimerHandle_t xTimer, TickType_t xNewPeriod, TickType_t)
{
    if (!ValidTimer(xTimer) || xNewPeriod == 0) return pdFAIL;
    xTimer->period = xNewPeriod;
    Start(xTimer, nullptr);  // FreeRTOS starts a dormant timer too
    k::PreemptionPoint();
    return pdPASS;
}

BaseType_t xTimerDelete(TimerHandle_t xTimer, TickType_t)
{
    if (!ValidTimer(xTimer)) return pdFAIL;
    tmrTimerControl** link = &g_timers;
    while (*link != nullptr && *link != xTimer) link = &(*link)->next;
    if (*link == xTimer) *link = xTimer->next;
    xTimer->magic = 0;
    if (xTimer->ownsObject) std::free(xTimer);
    return pdPASS;
}

BaseType_t xTimerStartFromISR(TimerHandle_t xTimer, BaseType_t* pxHigherPriorityTaskWoken)
{
    if (!ValidTimer(xTimer)) return pdFAIL;
    int woken = 0;
    Start(xTimer, &woken);
    if (woken && pxHigherPriorityTaskWoken != nullptr) *pxHigherPriorityTaskWoken = pdTRUE;
    return pdPASS;
}

BaseType_t xTimerResetFromISR(TimerHandle_t xTimer, BaseType_t* pxHigherPriorityTaskWoken)
{
    return xTimerStartFromISR(xTimer, pxHigherPriorityTaskWoken);
}

BaseType_t xTimerStopFromISR(TimerHandle_t xTimer, BaseType_t*)
{
    if (!ValidTimer(xTimer)) return pdFAIL;
    xTimer->active = false;
    return pdPASS;
}

BaseType_t xTimerChangePeriodFromISR(TimerHandle_t xTimer, TickType_t xNewPeriod, BaseType_t* pxHigherPriorityTaskWoken)
{
    if (!ValidTimer(xTimer) || xNewPeriod == 0) return pdFAIL;
    xTimer->period = xNewPeriod;
    return xTimerStartFromISR(xTimer, pxHigherPriorityTaskWoken);
}

}  // extern "C"

// ---- stream and message buffers -------------------------------------------------------------------

struct StreamBufferDef_t {
    uint32_t magic;
    uint8_t* storage;
    size_t size;  // storage bytes
    size_t head;
    size_t count;
    size_t trigger;
    bool message;
    bool ownsObject;
    bool ownsStorage;
    k::WaitQueue readers;
    k::WaitQueue writers;
    StreamBufferCallbackFunction_t sendDone;
    StreamBufferCallbackFunction_t receiveDone;
};
static_assert(sizeof(StreamBufferDef_t) <= sizeof(StaticStreamBuffer_t), "StaticStreamBuffer_t must hold a stream buffer");

namespace {

constexpr uint32_t kStreamMagic = 0x5354524D;
using MessageLength = uint32_t;  // FreeRTOS: configMESSAGE_BUFFER_LENGTH_TYPE (size_t on ESP-IDF, 4 bytes)

bool ValidStream(const StreamBufferDef_t* stream) { return stream != nullptr && stream->magic == kStreamMagic; }

void Put(StreamBufferDef_t* stream, const void* data, size_t bytes)
{
    const auto* in = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < bytes; ++index)
        stream->storage[(stream->head + stream->count + index) % stream->size] = in[index];
    stream->count += bytes;
}

void Get(StreamBufferDef_t* stream, void* data, size_t bytes, bool remove)
{
    auto* out = static_cast<uint8_t*>(data);
    for (size_t index = 0; index < bytes; ++index)
        if (out != nullptr) out[index] = stream->storage[(stream->head + index) % stream->size];
    if (!remove) return;
    stream->head = (stream->head + bytes) % stream->size;
    stream->count -= bytes;
}

size_t NextMessageBytes(StreamBufferDef_t* stream)
{
    if (!stream->message) return stream->count;
    if (stream->count < sizeof(MessageLength)) return 0;
    MessageLength length = 0;
    Get(stream, &length, sizeof(length), false);
    return length;
}

// Sends what fits now: a message only whole, stream bytes as many as there is room for.
size_t SendNow(StreamBufferDef_t* stream, const void* data, size_t bytes, int* woken)
{
    const size_t space = stream->size - stream->count;
    size_t sent = 0;
    if (stream->message)
    {
        if (space < bytes + sizeof(MessageLength)) return 0;
        const MessageLength length = static_cast<MessageLength>(bytes);
        Put(stream, &length, sizeof(length));
        Put(stream, data, bytes);
        sent = bytes;
    }
    else
    {
        sent = bytes < space ? bytes : space;
        Put(stream, data, sent);
    }
    if (sent > 0 && (stream->message || stream->count >= stream->trigger))
    {
        k::WakeOne(&stream->readers, 0, woken);
        if (stream->sendDone != nullptr) stream->sendDone(stream, k::InIsr(), nullptr);
    }
    return sent;
}

size_t ReceiveNow(StreamBufferDef_t* stream, void* data, size_t bytes, int* woken)
{
    size_t got = 0;
    if (stream->message)
    {
        const size_t length = NextMessageBytes(stream);
        if (length == 0 && stream->count == 0) return 0;
        if (length > bytes) return 0;  // the caller's buffer is too small: the message stays
        Get(stream, nullptr, sizeof(MessageLength), true);
        Get(stream, data, length, true);
        got = length;
    }
    else
    {
        got = bytes < stream->count ? bytes : stream->count;
        Get(stream, data, got, true);
    }
    if (got > 0)
    {
        k::WakeOne(&stream->writers, 0, woken);
        if (stream->receiveDone != nullptr) stream->receiveDone(stream, k::InIsr(), nullptr);
    }
    return got;
}

StreamBufferDef_t* CreateStream(size_t bytes, size_t trigger, bool message, uint8_t* storage, StaticStreamBuffer_t* buffer,
                                StreamBufferCallbackFunction_t sendDone, StreamBufferCallbackFunction_t receiveDone)
{
    cardenza_web::freertos::EnsureStarted();
    if (bytes == 0) return nullptr;
    void* memory = buffer != nullptr ? static_cast<void*>(buffer) : std::malloc(sizeof(StreamBufferDef_t));
    if (memory == nullptr) return nullptr;
    auto* stream = new (memory) StreamBufferDef_t{};
    stream->magic = kStreamMagic;
    stream->size = bytes;
    stream->trigger = trigger == 0 ? 1 : trigger;
    stream->message = message;
    stream->ownsObject = buffer == nullptr;
    stream->sendDone = sendDone;
    stream->receiveDone = receiveDone;
    stream->storage = storage;
    if (stream->storage == nullptr)
    {
        stream->storage = static_cast<uint8_t*>(std::malloc(bytes));
        stream->ownsStorage = true;
        if (stream->storage == nullptr)
        {
            stream->magic = 0;
            if (stream->ownsObject) std::free(stream);
            return nullptr;
        }
    }
    return stream;
}

}  // namespace

extern "C" {

StreamBufferHandle_t xStreamBufferGenericCreate(size_t xBufferSizeBytes, size_t xTriggerLevelBytes, BaseType_t xIsMessageBuffer,
                                                StreamBufferCallbackFunction_t pxSendCompletedCallback,
                                                StreamBufferCallbackFunction_t pxReceiveCompletedCallback)
{
    return CreateStream(xBufferSizeBytes, xTriggerLevelBytes, xIsMessageBuffer != pdFALSE, nullptr, nullptr, pxSendCompletedCallback,
                        pxReceiveCompletedCallback);
}

StreamBufferHandle_t xStreamBufferGenericCreateStatic(size_t xBufferSizeBytes, size_t xTriggerLevelBytes, BaseType_t xIsMessageBuffer,
                                                      uint8_t* const pucStreamBufferStorageArea,
                                                      StaticStreamBuffer_t* const pxStaticStreamBuffer,
                                                      StreamBufferCallbackFunction_t pxSendCompletedCallback,
                                                      StreamBufferCallbackFunction_t pxReceiveCompletedCallback)
{
    if (pucStreamBufferStorageArea == nullptr || pxStaticStreamBuffer == nullptr) return nullptr;
    return CreateStream(xBufferSizeBytes, xTriggerLevelBytes, xIsMessageBuffer != pdFALSE, pucStreamBufferStorageArea, pxStaticStreamBuffer,
                        pxSendCompletedCallback, pxReceiveCompletedCallback);
}

size_t xStreamBufferSend(StreamBufferHandle_t xStreamBuffer, const void* pvTxData, size_t xDataLengthBytes, TickType_t xTicksToWait)
{
    if (!ValidStream(xStreamBuffer)) return 0;
    const auto* data = static_cast<const uint8_t*>(pvTxData);
    size_t total = 0;
    const uint64_t deadline = k::DeadlineForTicks(xTicksToWait);
    for (;;)
    {
        total += SendNow(xStreamBuffer, data + total, xDataLengthBytes - total, nullptr);
        if (total == xDataLengthBytes || xTicksToWait == 0 || !k::InTask()) break;
        if (xStreamBuffer->message && total != 0) break;
        if (!k::Block(&xStreamBuffer->writers, deadline))
        {
            total += SendNow(xStreamBuffer, data + total, xDataLengthBytes - total, nullptr);
            break;
        }
    }
    k::PreemptionPoint();
    return total;
}

size_t xStreamBufferSendFromISR(StreamBufferHandle_t xStreamBuffer, const void* pvTxData, size_t xDataLengthBytes,
                                BaseType_t* const pxHigherPriorityTaskWoken)
{
    if (!ValidStream(xStreamBuffer)) return 0;
    int woken = 0;
    const size_t sent = SendNow(xStreamBuffer, pvTxData, xDataLengthBytes, &woken);
    if (woken && pxHigherPriorityTaskWoken != nullptr) *pxHigherPriorityTaskWoken = pdTRUE;
    k::YieldFromIsrRequest(woken);
    return sent;
}

size_t xStreamBufferReceive(StreamBufferHandle_t xStreamBuffer, void* pvRxData, size_t xBufferLengthBytes, TickType_t xTicksToWait)
{
    if (!ValidStream(xStreamBuffer)) return 0;
    const uint64_t deadline = k::DeadlineForTicks(xTicksToWait);
    for (;;)
    {
        const bool ready = xStreamBuffer->message ? xStreamBuffer->count > 0 : xStreamBuffer->count >= xStreamBuffer->trigger;
        if (ready || xTicksToWait == 0 || !k::InTask()) break;
        if (!k::Block(&xStreamBuffer->readers, deadline)) break;
    }
    const size_t got = ReceiveNow(xStreamBuffer, pvRxData, xBufferLengthBytes, nullptr);
    k::PreemptionPoint();
    return got;
}

size_t xStreamBufferReceiveFromISR(StreamBufferHandle_t xStreamBuffer, void* pvRxData, size_t xBufferLengthBytes,
                                   BaseType_t* const pxHigherPriorityTaskWoken)
{
    if (!ValidStream(xStreamBuffer)) return 0;
    int woken = 0;
    const size_t got = ReceiveNow(xStreamBuffer, pvRxData, xBufferLengthBytes, &woken);
    if (woken && pxHigherPriorityTaskWoken != nullptr) *pxHigherPriorityTaskWoken = pdTRUE;
    k::YieldFromIsrRequest(woken);
    return got;
}

void vStreamBufferDelete(StreamBufferHandle_t xStreamBuffer)
{
    if (!ValidStream(xStreamBuffer)) return;
    xStreamBuffer->magic = 0;
    if (xStreamBuffer->ownsStorage) std::free(xStreamBuffer->storage);
    if (xStreamBuffer->ownsObject) std::free(xStreamBuffer);
}

BaseType_t xStreamBufferIsFull(StreamBufferHandle_t xStreamBuffer)
{
    if (!ValidStream(xStreamBuffer)) return pdFALSE;
    const size_t reserve = xStreamBuffer->message ? sizeof(MessageLength) : 0;
    return xStreamBuffer->size - xStreamBuffer->count <= reserve ? pdTRUE : pdFALSE;
}

BaseType_t xStreamBufferIsEmpty(StreamBufferHandle_t xStreamBuffer)
{
    return ValidStream(xStreamBuffer) && xStreamBuffer->count == 0 ? pdTRUE : pdFALSE;
}

BaseType_t xStreamBufferReset(StreamBufferHandle_t xStreamBuffer)
{
    if (!ValidStream(xStreamBuffer) || !xStreamBuffer->readers.Empty() || !xStreamBuffer->writers.Empty()) return pdFAIL;
    xStreamBuffer->head = xStreamBuffer->count = 0;
    return pdPASS;
}

size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t xStreamBuffer)
{
    return ValidStream(xStreamBuffer) ? xStreamBuffer->size - xStreamBuffer->count : 0;
}

size_t xStreamBufferBytesAvailable(StreamBufferHandle_t xStreamBuffer)
{
    return ValidStream(xStreamBuffer) ? xStreamBuffer->count : 0;
}

BaseType_t xStreamBufferSetTriggerLevel(StreamBufferHandle_t xStreamBuffer, size_t xTriggerLevel)
{
    if (!ValidStream(xStreamBuffer) || xTriggerLevel > xStreamBuffer->size) return pdFALSE;
    xStreamBuffer->trigger = xTriggerLevel == 0 ? 1 : xTriggerLevel;
    return pdTRUE;
}

size_t xStreamBufferNextMessageLengthBytes(StreamBufferHandle_t xStreamBuffer)
{
    return ValidStream(xStreamBuffer) ? NextMessageBytes(xStreamBuffer) : 0;
}

}  // extern "C"
