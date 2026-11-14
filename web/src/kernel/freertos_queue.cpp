// FreeRTOS queues, semaphores, mutexes (with priority inheritance) and queue sets.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "kernel.h"
#include "raw_alloc.h"

namespace k = cardenza_web::kernel;

namespace cardenza_web::freertos {
void EnsureStarted();
}

struct QueueDefinition {
    uint32_t magic;
    uint8_t type;
    bool ownsObject;
    bool ownsStorage;
    UBaseType_t length;
    UBaseType_t itemSize;
    uint8_t* storage;
    UBaseType_t count;
    UBaseType_t readIndex;
    UBaseType_t writeIndex;
    k::WaitQueue senders;
    k::WaitQueue receivers;
    k::Task* holder;
    UBaseType_t recursion;
    QueueDefinition* set;
    const char* name;
    StaticQueue_t* staticBuffer;
};
static_assert(sizeof(QueueDefinition) <= sizeof(StaticQueue_t), "StaticQueue_t must hold a queue");

namespace {

constexpr uint32_t kQueueMagic = 0x51554555;
// The holder of a mutex taken outside any task (before the scheduler runs, or from the page's side):
// FreeRTOS lets that context give it back.
k::Task* const kNoTask = reinterpret_cast<k::Task*>(static_cast<uintptr_t>(1));

k::Task* Me()
{
    k::Task* task = k::Current();
    return task != nullptr ? task : kNoTask;
}

bool RealTask(const k::Task* task) { return task != nullptr && task != kNoTask; }

bool Valid(const QueueDefinition* queue) { return queue != nullptr && queue->magic == kQueueMagic; }

bool IsMutex(const QueueDefinition* queue)
{
    return queue->type == queueQUEUE_TYPE_MUTEX || queue->type == queueQUEUE_TYPE_RECURSIVE_MUTEX;
}

QueueDefinition* Create(UBaseType_t length, UBaseType_t itemSize, uint8_t type, uint8_t* storage, StaticQueue_t* buffer)
{
    cardenza_web::freertos::EnsureStarted();
    if (length == 0) return nullptr;
    void* memory = buffer;
    if (memory == nullptr) memory = std::malloc(sizeof(QueueDefinition));  // the firmware's heap, as on the device
    if (memory == nullptr) return nullptr;
    auto* queue = new (memory) QueueDefinition{};
    queue->magic = kQueueMagic;
    queue->type = type;
    queue->ownsObject = buffer == nullptr;
    queue->staticBuffer = buffer;
    queue->length = length;
    queue->itemSize = itemSize;
    if (itemSize != 0)
    {
        if (storage != nullptr) queue->storage = storage;
        else
        {
            queue->storage = static_cast<uint8_t*>(std::malloc(static_cast<std::size_t>(length) * itemSize));
            queue->ownsStorage = true;
            if (queue->storage == nullptr)
            {
                queue->magic = 0;
                if (queue->ownsObject) std::free(queue);
                return nullptr;
            }
        }
    }
    return queue;
}

// The set this queue belongs to hears that it has something.
void NotifySet(QueueDefinition* queue, int* woken)
{
    QueueDefinition* set = queue->set;
    if (set == nullptr || set->count >= set->length) return;
    std::memcpy(set->storage + set->writeIndex * set->itemSize, &queue, sizeof(queue));
    set->writeIndex = (set->writeIndex + 1) % set->length;
    ++set->count;
    k::WakeOne(&set->receivers, 0, woken);
}

void CopyIn(QueueDefinition* queue, const void* item, BaseType_t position)
{
    if (queue->itemSize == 0)
    {
        ++queue->count;
        return;
    }
    if (position == queueOVERWRITE && queue->count == queue->length)
    {
        // Overwrite is for a queue of one: replace what is there.
        const UBaseType_t last = (queue->writeIndex + queue->length - 1) % queue->length;
        std::memcpy(queue->storage + last * queue->itemSize, item, queue->itemSize);
        return;
    }
    if (position == queueSEND_TO_FRONT)
    {
        queue->readIndex = (queue->readIndex + queue->length - 1) % queue->length;
        std::memcpy(queue->storage + queue->readIndex * queue->itemSize, item, queue->itemSize);
    }
    else
    {
        std::memcpy(queue->storage + queue->writeIndex * queue->itemSize, item, queue->itemSize);
        queue->writeIndex = (queue->writeIndex + 1) % queue->length;
    }
    ++queue->count;
}

void CopyOut(QueueDefinition* queue, void* buffer, bool remove)
{
    if (queue->itemSize != 0 && buffer != nullptr)
        std::memcpy(buffer, queue->storage + queue->readIndex * queue->itemSize, queue->itemSize);
    if (!remove) return;
    if (queue->itemSize != 0) queue->readIndex = (queue->readIndex + 1) % queue->length;
    --queue->count;
}

unsigned HighestWaiter(const QueueDefinition* queue)
{
    return queue->receivers.head != nullptr ? queue->receivers.head->priority : 0;
}

void Disinherit(k::Task* holder, const QueueDefinition* mutex)
{
    if (holder->mutexesHeld > 0) --holder->mutexesHeld;
    unsigned priority = holder->basePriority;
    if (holder->mutexesHeld > 0 && mutex != nullptr) priority = std::max(priority, HighestWaiter(mutex));
    if (holder->priority != priority) k::SetPriority(holder, priority);
}

BaseType_t GiveMutex(QueueDefinition* queue)
{
    k::Task* current = Me();
    if (queue->holder == nullptr || queue->holder != current) return pdFAIL;
    queue->holder = nullptr;
    queue->recursion = 0;
    queue->count = 1;
    if (RealTask(current)) Disinherit(current, nullptr);
    NotifySet(queue, nullptr);
    k::WakeOne(&queue->receivers);
    k::PreemptionPoint();
    return pdPASS;
}

void TakeMutex(QueueDefinition* queue, k::Task* task)
{
    queue->count = 0;
    queue->holder = task;
    queue->recursion = 1;
    if (RealTask(task)) ++task->mutexesHeld;
}

BaseType_t Receive(QueueDefinition* queue, void* buffer, TickType_t ticks, bool remove)
{
    if (!Valid(queue)) return pdFAIL;
    uint64_t deadline = 0;
    bool deadlineSet = false;
    for (;;)
    {
        if (queue->count > 0)
        {
            if (IsMutex(queue)) TakeMutex(queue, Me());
            else CopyOut(queue, buffer, remove);
            if (remove) k::WakeOne(&queue->senders);
            else k::WakeOne(&queue->receivers);  // a peek leaves it for the next reader
            k::PreemptionPoint();
            return pdPASS;
        }
        if (ticks == 0 || !k::InTask())
        {
            k::PreemptionPoint();
            return errQUEUE_EMPTY;
        }
        if (!deadlineSet)
        {
            deadline = k::DeadlineForTicks(ticks);
            deadlineSet = true;
        }
        k::Task* current = k::Current();
        if (IsMutex(queue) && RealTask(queue->holder) && queue->holder->priority < current->priority)
            k::SetPriority(queue->holder, current->priority);  // priority inheritance
        const bool woken = k::Block(&queue->receivers, deadline);
        if (!woken && queue->count == 0)
        {
            if (IsMutex(queue) && RealTask(queue->holder) && queue->holder->priority > queue->holder->basePriority)
            {
                const unsigned rest = std::max(queue->holder->basePriority, HighestWaiter(queue));
                if (rest < queue->holder->priority) k::SetPriority(queue->holder, rest);
            }
            return errQUEUE_EMPTY;
        }
    }
}

}  // namespace

extern "C" {

QueueHandle_t xQueueGenericCreate(const UBaseType_t uxQueueLength, const UBaseType_t uxItemSize, const uint8_t ucQueueType)
{
    return Create(uxQueueLength, uxItemSize, ucQueueType, nullptr, nullptr);
}

QueueHandle_t xQueueGenericCreateStatic(const UBaseType_t uxQueueLength, const UBaseType_t uxItemSize, uint8_t* pucQueueStorage,
                                        StaticQueue_t* pxStaticQueue, const uint8_t ucQueueType)
{
    if (pxStaticQueue == nullptr || (uxItemSize != 0 && pucQueueStorage == nullptr)) return nullptr;
    return Create(uxQueueLength, uxItemSize, ucQueueType, pucQueueStorage, pxStaticQueue);
}

BaseType_t xQueueGenericGetStaticBuffers(QueueHandle_t xQueue, uint8_t** ppucQueueStorage, StaticQueue_t** ppxStaticQueue)
{
    if (!Valid(xQueue) || xQueue->staticBuffer == nullptr) return pdFALSE;
    if (ppucQueueStorage != nullptr) *ppucQueueStorage = xQueue->storage;
    if (ppxStaticQueue != nullptr) *ppxStaticQueue = xQueue->staticBuffer;
    return pdTRUE;
}

BaseType_t xQueueGenericSend(QueueHandle_t xQueue, const void* const pvItemToQueue, TickType_t xTicksToWait, const BaseType_t xCopyPosition)
{
    QueueDefinition* queue = xQueue;
    if (!Valid(queue)) return pdFAIL;
    if (IsMutex(queue)) return GiveMutex(queue);
    uint64_t deadline = 0;
    bool deadlineSet = false;
    for (;;)
    {
        if (queue->count < queue->length || xCopyPosition == queueOVERWRITE)
        {
            CopyIn(queue, pvItemToQueue, xCopyPosition);
            NotifySet(queue, nullptr);
            k::WakeOne(&queue->receivers);
            k::PreemptionPoint();
            return pdPASS;
        }
        if (xTicksToWait == 0 || !k::InTask())
        {
            k::PreemptionPoint();
            return errQUEUE_FULL;
        }
        if (!deadlineSet)
        {
            deadline = k::DeadlineForTicks(xTicksToWait);
            deadlineSet = true;
        }
        if (!k::Block(&queue->senders, deadline) && queue->count >= queue->length) return errQUEUE_FULL;
    }
}

BaseType_t xQueueGenericSendFromISR(QueueHandle_t xQueue, const void* const pvItemToQueue, BaseType_t* const pxHigherPriorityTaskWoken,
                                    const BaseType_t xCopyPosition)
{
    QueueDefinition* queue = xQueue;
    if (!Valid(queue)) return pdFAIL;
    if (queue->count >= queue->length && xCopyPosition != queueOVERWRITE) return errQUEUE_FULL;
    int woken = 0;
    CopyIn(queue, pvItemToQueue, xCopyPosition);
    NotifySet(queue, &woken);
    k::WakeOne(&queue->receivers, 0, &woken);
    if (woken && pxHigherPriorityTaskWoken != nullptr) *pxHigherPriorityTaskWoken = pdTRUE;
    k::YieldFromIsrRequest(woken);
    return pdPASS;
}

BaseType_t xQueueGiveFromISR(QueueHandle_t xQueue, BaseType_t* const pxHigherPriorityTaskWoken)
{
    return xQueueGenericSendFromISR(xQueue, nullptr, pxHigherPriorityTaskWoken, queueSEND_TO_BACK);
}

BaseType_t xQueueReceive(QueueHandle_t xQueue, void* const pvBuffer, TickType_t xTicksToWait)
{
    return Receive(xQueue, pvBuffer, xTicksToWait, true);
}

BaseType_t xQueuePeek(QueueHandle_t xQueue, void* const pvBuffer, TickType_t xTicksToWait)
{
    return Receive(xQueue, pvBuffer, xTicksToWait, false);
}

BaseType_t xQueuePeekFromISR(QueueHandle_t xQueue, void* const pvBuffer)
{
    if (!Valid(xQueue) || xQueue->count == 0) return pdFAIL;
    CopyOut(xQueue, pvBuffer, false);
    return pdPASS;
}

BaseType_t xQueueReceiveFromISR(QueueHandle_t xQueue, void* const pvBuffer, BaseType_t* const pxHigherPriorityTaskWoken)
{
    if (!Valid(xQueue) || xQueue->count == 0) return pdFAIL;
    int woken = 0;
    CopyOut(xQueue, pvBuffer, true);
    k::WakeOne(&xQueue->senders, 0, &woken);
    if (woken && pxHigherPriorityTaskWoken != nullptr) *pxHigherPriorityTaskWoken = pdTRUE;
    k::YieldFromIsrRequest(woken);
    return pdPASS;
}

BaseType_t xQueueSemaphoreTake(QueueHandle_t xQueue, TickType_t xTicksToWait)
{
    return Receive(xQueue, nullptr, xTicksToWait, true);
}

UBaseType_t uxQueueMessagesWaiting(const QueueHandle_t xQueue)
{
    k::PreemptionPoint();
    return Valid(xQueue) ? xQueue->count : 0;
}

UBaseType_t uxQueueMessagesWaitingFromISR(const QueueHandle_t xQueue) { return Valid(xQueue) ? xQueue->count : 0; }

UBaseType_t uxQueueSpacesAvailable(const QueueHandle_t xQueue)
{
    return Valid(xQueue) ? xQueue->length - xQueue->count : 0;
}

void vQueueDelete(QueueHandle_t xQueue)
{
    if (!Valid(xQueue)) return;
    xQueue->magic = 0;
    if (xQueue->ownsStorage) std::free(xQueue->storage);
    if (xQueue->ownsObject) std::free(xQueue);
}

void vQueueDeleteWithCaps(QueueHandle_t xQueue) { vQueueDelete(xQueue); }

QueueHandle_t xQueueCreateWithCaps(UBaseType_t uxQueueLength, UBaseType_t uxItemSize, UBaseType_t)
{
    return xQueueGenericCreate(uxQueueLength, uxItemSize, queueQUEUE_TYPE_BASE);
}

BaseType_t xQueueGenericReset(QueueHandle_t xQueue, BaseType_t xNewQueue)
{
    if (!Valid(xQueue)) return pdFAIL;
    xQueue->count = 0;
    xQueue->readIndex = xQueue->writeIndex = 0;
    if (!xNewQueue) k::WakeOne(&xQueue->senders);
    k::PreemptionPoint();
    return pdPASS;
}

BaseType_t xQueueIsQueueEmptyFromISR(const QueueHandle_t xQueue) { return Valid(xQueue) && xQueue->count == 0; }
BaseType_t xQueueIsQueueFullFromISR(const QueueHandle_t xQueue) { return Valid(xQueue) && xQueue->count >= xQueue->length; }

QueueHandle_t xQueueCreateMutex(const uint8_t ucQueueType)
{
    QueueDefinition* queue = Create(1, 0, ucQueueType, nullptr, nullptr);
    if (queue != nullptr) queue->count = 1;
    return queue;
}

QueueHandle_t xQueueCreateMutexStatic(const uint8_t ucQueueType, StaticQueue_t* pxStaticQueue)
{
    if (pxStaticQueue == nullptr) return nullptr;
    QueueDefinition* queue = Create(1, 0, ucQueueType, nullptr, pxStaticQueue);
    if (queue != nullptr) queue->count = 1;
    return queue;
}

QueueHandle_t xQueueCreateCountingSemaphore(const UBaseType_t uxMaxCount, const UBaseType_t uxInitialCount)
{
    if (uxInitialCount > uxMaxCount) return nullptr;
    QueueDefinition* queue = Create(uxMaxCount, 0, queueQUEUE_TYPE_COUNTING_SEMAPHORE, nullptr, nullptr);
    if (queue != nullptr) queue->count = uxInitialCount;
    return queue;
}

QueueHandle_t xQueueCreateCountingSemaphoreStatic(const UBaseType_t uxMaxCount, const UBaseType_t uxInitialCount, StaticQueue_t* pxStaticQueue)
{
    if (uxInitialCount > uxMaxCount || pxStaticQueue == nullptr) return nullptr;
    QueueDefinition* queue = Create(uxMaxCount, 0, queueQUEUE_TYPE_COUNTING_SEMAPHORE, nullptr, pxStaticQueue);
    if (queue != nullptr) queue->count = uxInitialCount;
    return queue;
}

TaskHandle_t xQueueGetMutexHolder(QueueHandle_t xSemaphore)
{
    if (!Valid(xSemaphore) || !IsMutex(xSemaphore) || !RealTask(xSemaphore->holder)) return nullptr;
    return reinterpret_cast<TaskHandle_t>(xSemaphore->holder);
}

TaskHandle_t xQueueGetMutexHolderFromISR(QueueHandle_t xSemaphore) { return xQueueGetMutexHolder(xSemaphore); }

BaseType_t xQueueTakeMutexRecursive(QueueHandle_t xMutex, TickType_t xTicksToWait)
{
    if (!Valid(xMutex)) return pdFAIL;
    if (xMutex->holder != nullptr && xMutex->holder == Me())
    {
        ++xMutex->recursion;
        return pdPASS;
    }
    return Receive(xMutex, nullptr, xTicksToWait, true);
}

BaseType_t xQueueGiveMutexRecursive(QueueHandle_t xMutex)
{
    if (!Valid(xMutex) || xMutex->holder == nullptr || xMutex->holder != Me()) return pdFAIL;
    if (--xMutex->recursion > 0) return pdPASS;
    xMutex->recursion = 1;
    return GiveMutex(xMutex);
}

void vQueueAddToRegistry(QueueHandle_t xQueue, const char* pcQueueName)
{
    if (Valid(xQueue)) xQueue->name = pcQueueName;
}

void vQueueUnregisterQueue(QueueHandle_t xQueue)
{
    if (Valid(xQueue)) xQueue->name = nullptr;
}

const char* pcQueueGetName(QueueHandle_t xQueue) { return Valid(xQueue) ? xQueue->name : nullptr; }

QueueSetHandle_t xQueueCreateSet(const UBaseType_t uxEventQueueLength)
{
    return Create(uxEventQueueLength, sizeof(QueueDefinition*), queueQUEUE_TYPE_SET, nullptr, nullptr);
}

BaseType_t xQueueAddToSet(QueueSetMemberHandle_t xQueueOrSemaphore, QueueSetHandle_t xQueueSet)
{
    if (!Valid(xQueueOrSemaphore) || !Valid(xQueueSet) || xQueueOrSemaphore->set != nullptr) return pdFAIL;
    if (xQueueOrSemaphore->count != 0 && !IsMutex(xQueueOrSemaphore)) return pdFAIL;
    xQueueOrSemaphore->set = xQueueSet;
    return pdPASS;
}

BaseType_t xQueueRemoveFromSet(QueueSetMemberHandle_t xQueueOrSemaphore, QueueSetHandle_t xQueueSet)
{
    if (!Valid(xQueueOrSemaphore) || xQueueOrSemaphore->set != xQueueSet) return pdFAIL;
    if (xQueueOrSemaphore->count != 0 && !IsMutex(xQueueOrSemaphore)) return pdFAIL;
    xQueueOrSemaphore->set = nullptr;
    return pdPASS;
}

QueueSetMemberHandle_t xQueueSelectFromSet(QueueSetHandle_t xQueueSet, const TickType_t xTicksToWait)
{
    QueueDefinition* member = nullptr;
    if (xQueueReceive(xQueueSet, &member, xTicksToWait) != pdPASS) return nullptr;
    return member;
}

QueueSetMemberHandle_t xQueueSelectFromSetFromISR(QueueSetHandle_t xQueueSet)
{
    QueueDefinition* member = nullptr;
    if (xQueueReceiveFromISR(xQueueSet, &member, nullptr) != pdPASS) return nullptr;
    return member;
}

}  // extern "C"
