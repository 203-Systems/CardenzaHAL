// FreeRTOS tasks, notifications and the port layer (critical sections, core identity) on the web
// target's kernel.

#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "kernel.h"
#include "../esp/heap_internal.h"

namespace k = cardenza_web::kernel;

namespace {

k::Task* ToTask(TaskHandle_t handle)
{
    return reinterpret_cast<k::Task*>(handle);
}

TaskHandle_t ToHandle(k::Task* task)
{
    return reinterpret_cast<TaskHandle_t>(task);
}

k::Task* OrCurrent(TaskHandle_t handle)
{
    return handle != nullptr ? ToTask(handle) : k::Current();
}

k::Task* g_idle[2] = {};

void IdleEntry(void*)
{
    for (;;) k::Block(nullptr, k::kForever);
}

void EnsureKernel()
{
    if (k::Started()) return;
    k::Init(configTICK_RATE_HZ);
    // The idle tasks exist for their handles (xTaskGetIdleTaskHandleForCore); they never run: the
    // scheduler returns to the browser when nothing is ready.
    for (int core = 0; core < 2; ++core)
    {
        char name[8];
        std::snprintf(name, sizeof(name), "IDLE%d", core);
        g_idle[core] = k::CreateTask(IdleEntry, nullptr, name, configMINIMAL_STACK_SIZE, 0, core, true);
        if (g_idle[core] != nullptr) k::Suspend(g_idle[core]);
    }
}

// Tasks take their stack from internal RAM on the device (a small TCB too).
constexpr uint32_t kTcbBytes = 352;

eTaskState StateOf(const k::Task* task)
{
    if (task == k::Current()) return eRunning;
    switch (task->state)
    {
        case k::TaskState::Running: return eRunning;
        case k::TaskState::Ready: return eReady;
        case k::TaskState::Blocked: return eBlocked;
        case k::TaskState::Suspended: return eSuspended;
        case k::TaskState::Deleted: return eDeleted;
    }
    return eInvalid;
}

configSTACK_DEPTH_TYPE HighWater(const k::Task* task)
{
    const std::size_t used = k::ContextStackUsed(&task->context);
    return used >= task->stackBytes ? 0 : static_cast<configSTACK_DEPTH_TYPE>(task->stackBytes - used);
}

}  // namespace

namespace cardenza_web::freertos {
void EnsureStarted() { EnsureKernel(); }
}

extern "C" {

void vAssertCalled(const char* file, int line)
{
    k::Fatal("assert failed in %s:%d", file, line);
}

// ---- creation ---------------------------------------------------------------------------------

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t pxTaskCode, const char* const pcName, const configSTACK_DEPTH_TYPE usStackDepth,
                                   void* const pvParameters, UBaseType_t uxPriority, TaskHandle_t* const pxCreatedTask,
                                   const BaseType_t xCoreID)
{
    EnsureKernel();
    if (k::InIsr()) return pdFAIL;
    void* charge = cardenza_web::heap::Charge(usStackDepth + kTcbBytes);
    if (charge == nullptr) return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    k::Task* task = k::CreateTask(pxTaskCode, pvParameters, pcName, usStackDepth, uxPriority & 0x7FFFFFFFu,
                                  xCoreID == tskNO_AFFINITY ? k::kNoAffinity : xCoreID);
    if (task == nullptr)
    {
        cardenza_web::heap::Refund(charge);
        return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    }
    task->heapCharge = charge;
    if (pxCreatedTask != nullptr) *pxCreatedTask = ToHandle(task);
    k::PreemptionPoint();
    return pdPASS;
}

TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t pxTaskCode, const char* const pcName, const uint32_t ulStackDepth,
                                           void* const pvParameters, UBaseType_t uxPriority, StackType_t* const,
                                           StaticTask_t* const, const BaseType_t xCoreID)
{
    EnsureKernel();
    // The firmware brought its own memory: nothing is charged to the heap.
    k::Task* task = k::CreateTask(pxTaskCode, pvParameters, pcName, ulStackDepth, uxPriority & 0x7FFFFFFFu,
                                  xCoreID == tskNO_AFFINITY ? k::kNoAffinity : xCoreID);
    if (task == nullptr) return nullptr;
    task->stackBytes = ulStackDepth;
    k::PreemptionPoint();
    return ToHandle(task);
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t pvTaskCode, const char* const pcName, const configSTACK_DEPTH_TYPE usStackDepth,
                                           void* const pvParameters, UBaseType_t uxPriority, TaskHandle_t* const pvCreatedTask,
                                           const BaseType_t xCoreID, UBaseType_t)
{
    return xTaskCreatePinnedToCore(pvTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pvCreatedTask, xCoreID);
}

void vTaskDelete(TaskHandle_t xTaskToDelete)
{
    k::Task* task = OrCurrent(xTaskToDelete);
    if (!k::ValidTask(task)) return;
    void* charge = task->heapCharge;
    task->heapCharge = nullptr;
    cardenza_web::heap::Refund(charge);  // only accounting: safe before a self-delete switches away
    k::DeleteTask(task);
    k::PreemptionPoint();
}

void vTaskDeleteWithCaps(TaskHandle_t xTaskToDelete) { vTaskDelete(xTaskToDelete); }

// ---- control ----------------------------------------------------------------------------------

void vTaskDelay(const TickType_t xTicksToDelay)
{
    if (xTicksToDelay == 0)
    {
        k::Yield();
        return;
    }
    k::SleepUntil(k::DeadlineForTicks(xTicksToDelay));
}

BaseType_t xTaskDelayUntil(TickType_t* const pxPreviousWakeTime, const TickType_t xTimeIncrement)
{
    const uint64_t nowTick64 = k::NowUs() / k::TickUs();
    const TickType_t now = static_cast<TickType_t>(nowTick64);
    const TickType_t wake = *pxPreviousWakeTime + xTimeIncrement;
    bool delay;
    if (now < *pxPreviousWakeTime) delay = wake < *pxPreviousWakeTime && wake > now;
    else delay = wake < *pxPreviousWakeTime || wake > now;
    *pxPreviousWakeTime = wake;
    if (delay)
    {
        const uint64_t deadline = (nowTick64 + static_cast<TickType_t>(wake - now)) * k::TickUs();
        k::SleepUntil(deadline);
    }
    else
    {
        k::PreemptionPoint();
    }
    return delay ? pdTRUE : pdFALSE;
}

BaseType_t xTaskAbortDelay(TaskHandle_t xTask)
{
    k::Task* task = ToTask(xTask);
    if (!k::ValidTask(task) || task->state != k::TaskState::Blocked) return pdFAIL;
    k::WakeTask(task, 0, nullptr);
    task->timedOut = true;  // the blocking call returns as if its time ran out
    k::PreemptionPoint();
    return pdPASS;
}

UBaseType_t uxTaskPriorityGet(const TaskHandle_t xTask)
{
    k::Task* task = OrCurrent(xTask);
    return k::ValidTask(task) ? task->priority : 0;
}

UBaseType_t uxTaskPriorityGetFromISR(const TaskHandle_t xTask) { return uxTaskPriorityGet(xTask); }

eTaskState eTaskGetState(TaskHandle_t xTask)
{
    k::Task* task = ToTask(xTask);
    return k::ValidTask(task) ? StateOf(task) : eDeleted;
}

void vTaskGetInfo(TaskHandle_t xTask, TaskStatus_t* status, BaseType_t xGetFreeStackSpace, eTaskState eState)
{
    k::Task* task = OrCurrent(xTask);
    if (!k::ValidTask(task) || status == nullptr) return;
    status->xHandle = ToHandle(task);
    status->pcTaskName = task->name;
    status->xTaskNumber = task->number & 0x7FFFFFFFu;
    status->eCurrentState = eState == eInvalid ? StateOf(task) : eState;
    status->uxCurrentPriority = task->priority;
    status->uxBasePriority = task->basePriority;
    status->ulRunTimeCounter = static_cast<uint32_t>(task->runTimeUs);
    status->pxStackBase = static_cast<StackType_t*>(task->context.stack);
    status->usStackHighWaterMark = xGetFreeStackSpace ? HighWater(task) : 0;
    status->xCoreID = task->core == k::kNoAffinity ? tskNO_AFFINITY : task->core;
}

void vTaskPrioritySet(TaskHandle_t xTask, UBaseType_t uxNewPriority)
{
    k::Task* task = OrCurrent(xTask);
    if (!k::ValidTask(task)) return;
    k::SetBasePriority(task, uxNewPriority);
    k::PreemptionPoint();
}

void vTaskSuspend(TaskHandle_t xTaskToSuspend)
{
    k::Task* task = OrCurrent(xTaskToSuspend);
    if (!k::ValidTask(task)) return;
    k::Suspend(task);
    k::PreemptionPoint();
}

void vTaskResume(TaskHandle_t xTaskToResume)
{
    k::Task* task = ToTask(xTaskToResume);
    if (!k::ValidTask(task)) return;
    k::Resume(task);
    k::PreemptionPoint();
}

BaseType_t xTaskResumeFromISR(TaskHandle_t xTaskToResume)
{
    k::Task* task = ToTask(xTaskToResume);
    return k::ValidTask(task) && k::Resume(task) ? pdTRUE : pdFALSE;
}

// ---- scheduler --------------------------------------------------------------------------------

void vTaskStartScheduler(void) { EnsureKernel(); }  // ESP-IDF starts it before app_main
void vTaskEndScheduler(void) {}
void vTaskSuspendAll(void) { k::SuspendAll(); }
BaseType_t xTaskResumeAll(void) { return k::ResumeAll() ? pdTRUE : pdFALSE; }

BaseType_t xTaskGetSchedulerState(void)
{
    if (!k::Started()) return taskSCHEDULER_NOT_STARTED;
    return k::SchedulerSuspended() ? taskSCHEDULER_SUSPENDED : taskSCHEDULER_RUNNING;
}

// ---- information ------------------------------------------------------------------------------

TickType_t xTaskGetTickCount(void)
{
    k::PreemptionPoint();
    return k::TickCount();
}

TickType_t xTaskGetTickCountFromISR(void) { return k::TickCount(); }
UBaseType_t uxTaskGetNumberOfTasks(void) { return k::TaskCount(); }

char* pcTaskGetName(TaskHandle_t xTaskToQuery)
{
    k::Task* task = OrCurrent(xTaskToQuery);
    return k::ValidTask(task) ? task->name : nullptr;
}

TaskHandle_t xTaskGetHandle(const char* pcNameToQuery)
{
    for (k::Task* task = k::AllTasks(); task != nullptr; task = task->allNext)
        if (std::strncmp(task->name, pcNameToQuery, sizeof(task->name)) == 0) return ToHandle(task);
    return nullptr;
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t xTask)
{
    k::Task* task = OrCurrent(xTask);
    return k::ValidTask(task) ? HighWater(task) : 0;
}

configSTACK_DEPTH_TYPE uxTaskGetStackHighWaterMark2(TaskHandle_t xTask) { return uxTaskGetStackHighWaterMark(xTask); }

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    k::Task* task = k::Current();
    if (task == nullptr && k::InIsr()) task = k::LastOnCore(k::CurrentCore());
    return ToHandle(task);
}

TaskHandle_t xTaskGetCurrentTaskHandleForCore(BaseType_t xCoreID)
{
    k::Task* current = k::Current();
    if (current != nullptr && k::CurrentCore() == xCoreID) return ToHandle(current);
    k::Task* last = k::LastOnCore(xCoreID);
    return last != nullptr ? ToHandle(last) : ToHandle(g_idle[xCoreID & 1]);
}

TaskHandle_t xTaskGetIdleTaskHandle(void) { return ToHandle(g_idle[k::CurrentCore() & 1]); }
TaskHandle_t xTaskGetIdleTaskHandleForCore(BaseType_t xCoreID) { return ToHandle(g_idle[xCoreID & 1]); }

BaseType_t xTaskGetCoreID(TaskHandle_t xTask)
{
    k::Task* task = OrCurrent(xTask);
    if (!k::ValidTask(task)) return tskNO_AFFINITY;
    return task->core == k::kNoAffinity ? tskNO_AFFINITY : task->core;
}

UBaseType_t uxTaskGetSystemState(TaskStatus_t* const pxTaskStatusArray, const UBaseType_t uxArraySize, uint32_t* const pulTotalRunTime)
{
    UBaseType_t count = 0;
    for (k::Task* task = k::AllTasks(); task != nullptr && count < uxArraySize; task = task->allNext)
    {
        vTaskGetInfo(ToHandle(task), &pxTaskStatusArray[count], pdTRUE, eInvalid);
        if (task == g_idle[0] || task == g_idle[1]) pxTaskStatusArray[count].ulRunTimeCounter = static_cast<uint32_t>(k::IdleUs() / 2);
        ++count;
    }
    if (pulTotalRunTime != nullptr) *pulTotalRunTime = static_cast<uint32_t>(k::NowUs());
    return count;
}

void vTaskList(char* pcWriteBuffer)
{
    char* out = pcWriteBuffer;
    for (k::Task* task = k::AllTasks(); task != nullptr; task = task->allNext)
    {
        const char state = "XRBSD"[StateOf(task) == eRunning ? 0 : StateOf(task)];
        out += std::sprintf(out, "%-16s%c\t%u\t%u\t%u\t%d\n", task->name, state, task->priority, static_cast<unsigned>(HighWater(task)),
                            static_cast<unsigned>(task->number), task->core == k::kNoAffinity ? -1 : task->core);
    }
    *out = '\0';
}

void vTaskGetRunTimeStats(char* pcWriteBuffer)
{
    char* out = pcWriteBuffer;
    const uint64_t total = k::NowUs() / 100 + 1;
    for (k::Task* task = k::AllTasks(); task != nullptr; task = task->allNext)
    {
        uint64_t runtime = task->runTimeUs;
        if (task == g_idle[0] || task == g_idle[1]) runtime = k::IdleUs() / 2;
        const unsigned percent = static_cast<unsigned>(runtime / total);
        out += std::sprintf(out, "%-16s%u\t\t%u%%\n", task->name, static_cast<unsigned>(runtime), percent);
    }
    *out = '\0';
}

uint32_t ulTaskGetIdleRunTimeCounter(void) { return static_cast<uint32_t>(k::IdleUs()); }

uint32_t ulTaskGetIdleRunTimePercent(void)
{
    const uint64_t now = k::NowUs();
    return now == 0 ? 100 : static_cast<uint32_t>(k::IdleUs() * 100 / now);
}

void vTaskSetApplicationTaskTag(TaskHandle_t, TaskHookFunction_t) {}
TaskHookFunction_t xTaskGetApplicationTaskTag(TaskHandle_t) { return nullptr; }
BaseType_t xTaskCallApplicationTaskHook(TaskHandle_t, void*) { return pdFAIL; }

void vTaskSetThreadLocalStoragePointer(TaskHandle_t xTaskToSet, BaseType_t xIndex, void* pvValue)
{
    k::Task* task = OrCurrent(xTaskToSet);
    if (!k::ValidTask(task) || xIndex < 0 || xIndex >= k::kTlsPointers) return;
    task->tls[xIndex] = pvValue;
}

void* pvTaskGetThreadLocalStoragePointer(TaskHandle_t xTaskToQuery, BaseType_t xIndex)
{
    k::Task* task = OrCurrent(xTaskToQuery);
    if (!k::ValidTask(task) || xIndex < 0 || xIndex >= k::kTlsPointers) return nullptr;
    return task->tls[xIndex];
}

void vTaskSetThreadLocalStoragePointerAndDelCallback(TaskHandle_t xTaskToSet, BaseType_t xIndex, void* pvValue,
                                                      TlsDeleteCallbackFunction_t pvDelCallback)
{
    k::Task* task = OrCurrent(xTaskToSet);
    if (!k::ValidTask(task) || xIndex < 0 || xIndex >= k::kTlsPointers) return;
    task->tls[xIndex] = pvValue;
    task->tlsDelete[xIndex] = pvDelCallback;
}

void vTaskSetTimeOutState(TimeOut_t* const pxTimeOut)
{
    pxTimeOut->xOverflowCount = 0;
    pxTimeOut->xTimeOnEntering = k::TickCount();
}

void vTaskInternalSetTimeOutState(TimeOut_t* const pxTimeOut) { vTaskSetTimeOutState(pxTimeOut); }

BaseType_t xTaskCheckForTimeOut(TimeOut_t* const pxTimeOut, TickType_t* const pxTicksToWait)
{
    if (*pxTicksToWait == portMAX_DELAY) return pdFALSE;
    const TickType_t now = k::TickCount();
    const TickType_t elapsed = now - pxTimeOut->xTimeOnEntering;
    if (elapsed >= *pxTicksToWait)
    {
        *pxTicksToWait = 0;
        return pdTRUE;
    }
    *pxTicksToWait -= elapsed;
    vTaskSetTimeOutState(pxTimeOut);
    return pdFALSE;
}

BaseType_t xTaskCatchUpTicks(TickType_t) { return pdFALSE; }

UBaseType_t uxTaskGetTaskNumber(TaskHandle_t xTask)
{
    k::Task* task = ToTask(xTask);
    return k::ValidTask(task) ? task->number : 0;
}

void vTaskSetTaskNumber(TaskHandle_t, const UBaseType_t) {}

uint8_t* pxTaskGetStackStart(TaskHandle_t xTask)
{
    k::Task* task = OrCurrent(xTask);
    return k::ValidTask(task) ? static_cast<uint8_t*>(task->context.stack) : nullptr;
}

// ---- notifications ----------------------------------------------------------------------------

namespace {

constexpr uint8_t kNotWaiting = 0;
constexpr uint8_t kWaiting = 1;
constexpr uint8_t kReceived = 2;

BaseType_t Notify(k::Task* task, UBaseType_t index, uint32_t value, eNotifyAction action, uint32_t* previous, int* woken)
{
    if (!k::ValidTask(task) || index >= static_cast<UBaseType_t>(k::kNotifyEntries)) return pdFAIL;
    if (previous != nullptr) *previous = task->notifyValue[index];
    const uint8_t before = task->notifyState[index];
    task->notifyState[index] = kReceived;
    switch (action)
    {
        case eSetBits: task->notifyValue[index] |= value; break;
        case eIncrement: ++task->notifyValue[index]; break;
        case eSetValueWithOverwrite: task->notifyValue[index] = value; break;
        case eSetValueWithoutOverwrite:
            if (before == kReceived) return pdFAIL;
            task->notifyValue[index] = value;
            break;
        case eNoAction: break;
    }
    if (before == kWaiting && task->notifyWaitIndex == static_cast<int>(index))
        k::WakeOne(&task->notifyWaiters, 0, woken);
    return pdPASS;
}

}  // namespace

BaseType_t xTaskGenericNotify(TaskHandle_t xTaskToNotify, UBaseType_t uxIndexToNotify, uint32_t ulValue, eNotifyAction eAction,
                              uint32_t* pulPreviousNotificationValue)
{
    const BaseType_t result = Notify(ToTask(xTaskToNotify), uxIndexToNotify, ulValue, eAction, pulPreviousNotificationValue, nullptr);
    k::PreemptionPoint();
    return result;
}

BaseType_t xTaskGenericNotifyFromISR(TaskHandle_t xTaskToNotify, UBaseType_t uxIndexToNotify, uint32_t ulValue, eNotifyAction eAction,
                                     uint32_t* pulPreviousNotificationValue, BaseType_t* pxHigherPriorityTaskWoken)
{
    int woken = 0;
    const BaseType_t result = Notify(ToTask(xTaskToNotify), uxIndexToNotify, ulValue, eAction, pulPreviousNotificationValue, &woken);
    if (woken && pxHigherPriorityTaskWoken != nullptr) *pxHigherPriorityTaskWoken = pdTRUE;
    k::YieldFromIsrRequest(woken);
    return result;
}

void vTaskGenericNotifyGiveFromISR(TaskHandle_t xTaskToNotify, UBaseType_t uxIndexToNotify, BaseType_t* pxHigherPriorityTaskWoken)
{
    (void)xTaskGenericNotifyFromISR(xTaskToNotify, uxIndexToNotify, 0, eIncrement, nullptr, pxHigherPriorityTaskWoken);
}

uint32_t ulTaskGenericNotifyTake(UBaseType_t uxIndexToWaitOn, BaseType_t xClearCountOnExit, TickType_t xTicksToWait)
{
    k::Task* task = k::Current();
    if (task == nullptr || uxIndexToWaitOn >= static_cast<UBaseType_t>(k::kNotifyEntries)) return 0;
    if (task->notifyValue[uxIndexToWaitOn] == 0 && xTicksToWait != 0)
    {
        task->notifyState[uxIndexToWaitOn] = kWaiting;
        task->notifyWaitIndex = static_cast<int>(uxIndexToWaitOn);
        k::Block(&task->notifyWaiters, k::DeadlineForTicks(xTicksToWait));
        task->notifyWaitIndex = -1;
    }
    else
    {
        k::PreemptionPoint();
    }
    const uint32_t value = task->notifyValue[uxIndexToWaitOn];
    if (value != 0) task->notifyValue[uxIndexToWaitOn] = xClearCountOnExit ? 0 : value - 1;
    task->notifyState[uxIndexToWaitOn] = kNotWaiting;
    return value;
}

BaseType_t xTaskGenericNotifyWait(UBaseType_t uxIndexToWaitOn, uint32_t ulBitsToClearOnEntry, uint32_t ulBitsToClearOnExit,
                                  uint32_t* pulNotificationValue, TickType_t xTicksToWait)
{
    k::Task* task = k::Current();
    if (task == nullptr || uxIndexToWaitOn >= static_cast<UBaseType_t>(k::kNotifyEntries)) return pdFALSE;
    if (task->notifyState[uxIndexToWaitOn] != kReceived)
    {
        task->notifyValue[uxIndexToWaitOn] &= ~ulBitsToClearOnEntry;
        if (xTicksToWait != 0)
        {
            task->notifyState[uxIndexToWaitOn] = kWaiting;
            task->notifyWaitIndex = static_cast<int>(uxIndexToWaitOn);
            k::Block(&task->notifyWaiters, k::DeadlineForTicks(xTicksToWait));
            task->notifyWaitIndex = -1;
        }
    }
    else
    {
        k::PreemptionPoint();
    }
    if (pulNotificationValue != nullptr) *pulNotificationValue = task->notifyValue[uxIndexToWaitOn];
    const bool received = task->notifyState[uxIndexToWaitOn] == kReceived;
    if (received) task->notifyValue[uxIndexToWaitOn] &= ~ulBitsToClearOnExit;
    task->notifyState[uxIndexToWaitOn] = kNotWaiting;
    return received ? pdTRUE : pdFALSE;
}

BaseType_t xTaskGenericNotifyStateClear(TaskHandle_t xTask, UBaseType_t uxIndexToClear)
{
    k::Task* task = OrCurrent(xTask);
    if (!k::ValidTask(task) || uxIndexToClear >= static_cast<UBaseType_t>(k::kNotifyEntries)) return pdFAIL;
    if (task->notifyState[uxIndexToClear] != kReceived) return pdFAIL;
    task->notifyState[uxIndexToClear] = kNotWaiting;
    return pdPASS;
}

uint32_t ulTaskGenericNotifyValueClear(TaskHandle_t xTask, UBaseType_t uxIndexToClear, uint32_t ulBitsToClear)
{
    k::Task* task = OrCurrent(xTask);
    if (!k::ValidTask(task) || uxIndexToClear >= static_cast<UBaseType_t>(k::kNotifyEntries)) return 0;
    const uint32_t value = task->notifyValue[uxIndexToClear];
    task->notifyValue[uxIndexToClear] &= ~ulBitsToClear;
    return value;
}

// ---- heap (FreeRTOS portable layer) -----------------------------------------------------------

size_t xPortGetFreeHeapSize(void) { return cardenza_web::heap::FreeInternalAndDefault(); }
size_t xPortGetMinimumEverFreeHeapSize(void) { return cardenza_web::heap::MinimumFreeInternalAndDefault(); }
void* pvPortMalloc(size_t xWantedSize) { return malloc(xWantedSize); }
void vPortFree(void* pv) { free(pv); }

// ---- port -------------------------------------------------------------------------------------

void spinlock_initialize(spinlock_t* lock)
{
    lock->owner = SPINLOCK_FREE;
    lock->count = 0;
}

bool spinlock_acquire(spinlock_t* lock, int32_t)
{
    ++lock->count;
    return true;
}

void spinlock_release(spinlock_t* lock)
{
    if (lock->count > 0) --lock->count;
}

void vPortEnterCritical(portMUX_TYPE* mux)
{
    if (mux != nullptr) ++mux->count;
    k::EnterCritical();
}

void vPortExitCritical(portMUX_TYPE* mux)
{
    if (mux != nullptr && mux->count > 0) --mux->count;
    k::ExitCritical();
}

BaseType_t xPortEnterCriticalTimeout(portMUX_TYPE* mux, BaseType_t)
{
    vPortEnterCritical(mux);
    return pdPASS;
}

void vPortEnterCriticalSafe(portMUX_TYPE* mux) { vPortEnterCritical(mux); }
void vPortExitCriticalSafe(portMUX_TYPE* mux) { vPortExitCritical(mux); }
BaseType_t xPortInIsrContext(void) { return k::InIsr() ? pdTRUE : pdFALSE; }
BaseType_t xPortInterruptedFromISRContext(void) { return k::InIsr() ? pdTRUE : pdFALSE; }
BaseType_t xPortGetCoreID(void) { return k::CurrentCore(); }
void vPortYield(void) { k::Yield(); }
void vPortYieldFromISR(void) { k::YieldFromIsrRequest(1); }
void vPortYieldOtherCore(BaseType_t) { k::PreemptionPoint(); }

UBaseType_t xPortSetInterruptMaskFromISR(void)
{
    k::EnterCritical();
    return 0;
}

void vPortClearInterruptMaskFromISR(UBaseType_t) { k::ExitCritical(); }
uint32_t xPortGetTickRateHz(void) { return k::TickRateHz(); }
void vPortSetStackWatchpoint(void*) {}
BaseType_t xPortCheckValidTCBMem(const void* ptr) { return ptr != nullptr; }
BaseType_t xPortcheckValidStackMem(const void* ptr) { return ptr != nullptr; }

}  // extern "C"
