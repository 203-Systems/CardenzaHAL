#include "kernel.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "raw_alloc.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

namespace cardenza_web::kernel {

namespace {

constexpr uint32_t kTaskMagic = 0x7A5C0DE5;
constexpr int kCores = 2;  // the ESP32-S3: two virtual cores, time-sliced on the one thread
// What a task's context gets beyond what the firmware asked for: wasm code keeps less on the C
// stack than Xtensa's register windows, but deep C++ and printf want room.
constexpr std::size_t kMinStackBytes = 64 * 1024;
constexpr std::size_t kAsyncBytes = 128 * 1024;

uint64_t (*g_testClock)() = nullptr;
uint64_t g_lastNowUs = 0;
uint32_t g_tickRateHz = 1000;
uint64_t g_tickUs = 1000;
bool g_started = false;

Context g_schedulerContext;
Task* g_current = nullptr;
int g_currentCore = 0;
Task* g_allTasks = nullptr;
Task* g_dead = nullptr;
unsigned g_taskCount = 0;
uint32_t g_taskNumber = 0;

Task* g_ready[kMaxPriorities] = {};  // circular lists
uint32_t g_readyMask = 0;

int g_isrDepth = 0;
int g_critical = 0;
int g_suspendAll = 0;
bool g_yieldPending = false;

TimedSource* g_sources = nullptr;
uint64_t g_nextEventUs = kForever;
bool g_nextEventDirty = true;

uint64_t g_runEnteredUs = 0;
uint64_t g_runBudgetUs = 8000;
uint64_t g_coreTimeUs[kCores] = {};
Task* g_lastOnCore[kCores] = {};
uint64_t g_busyUs = 0;
uint64_t g_hogWarningUs = 500000;
void (*g_idleHook)() = nullptr;

uint64_t PlatformNowUs()
{
#if defined(__EMSCRIPTEN__)
    static const double boot = emscripten_get_now();
    return static_cast<uint64_t>((emscripten_get_now() - boot) * 1000.0);
#else
    static const auto boot = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - boot).count());
#endif
}

// ---- ready lists ------------------------------------------------------------------------------

void ReadyInsert(Task* task, bool front)
{
    Task*& head = g_ready[task->priority];
    if (head == nullptr)
    {
        task->readyNext = task->readyPrev = task;
        head = task;
    }
    else
    {
        task->readyNext = head;
        task->readyPrev = head->readyPrev;
        head->readyPrev->readyNext = task;
        head->readyPrev = task;
        if (front) head = task;
    }
    g_readyMask |= 1u << task->priority;
}

void ReadyRemove(Task* task)
{
    Task*& head = g_ready[task->priority];
    if (task->readyNext == task)
    {
        head = nullptr;
        g_readyMask &= ~(1u << task->priority);
    }
    else
    {
        task->readyPrev->readyNext = task->readyNext;
        task->readyNext->readyPrev = task->readyPrev;
        if (head == task) head = task->readyNext;
    }
    task->readyNext = task->readyPrev = nullptr;
}

bool Eligible(const Task* task, int core)
{
    return task->core == kNoAffinity || task->core == core;
}

// The task core `core` would run next: the highest priority, first in its list, allowed on that core.
Task* PickFor(int core, unsigned minPriority = 0)
{
    uint32_t mask = g_readyMask;
    while (mask != 0)
    {
        const unsigned priority = 31u - static_cast<unsigned>(__builtin_clz(mask));
        if (priority < minPriority) return nullptr;
        Task* head = g_ready[priority];
        Task* task = head;
        do
        {
            if (Eligible(task, core)) return task;
            task = task->readyNext;
        } while (task != head);
        mask &= ~(1u << priority);
    }
    return nullptr;
}

void MakeReady(Task* task, bool front = false)
{
    task->state = TaskState::Ready;
    task->readySinceUs = g_lastNowUs;
    ReadyInsert(task, front);
}

// ---- wait queues ------------------------------------------------------------------------------

void WaitInsert(WaitQueue* queue, Task* task)
{
    Task** link = &queue->head;
    while (*link != nullptr && (*link)->priority >= task->priority) link = &(*link)->waitNext;
    task->waitNext = *link;
    *link = task;
    task->waitingOn = queue;
}

void WaitRemove(Task* task)
{
    if (task->waitingOn == nullptr) return;
    Task** link = &task->waitingOn->head;
    while (*link != nullptr && *link != task) link = &(*link)->waitNext;
    if (*link == task) *link = task->waitNext;
    task->waitNext = nullptr;
    task->waitingOn = nullptr;
}

// ---- events -----------------------------------------------------------------------------------

void RecomputeNextEvent()
{
    uint64_t next = kForever;
    for (Task* task = g_allTasks; task != nullptr; task = task->allNext)
        if (task->state == TaskState::Blocked && task->deadlineUs < next) next = task->deadlineUs;
    for (TimedSource* source = g_sources; source != nullptr; source = source->next)
        if (source->due < next) next = source->due;
    g_nextEventUs = next;
    g_nextEventDirty = false;
}

uint64_t NextEventUs()
{
    if (g_nextEventDirty) RecomputeNextEvent();
    return g_nextEventUs;
}

void ProcessEvents(uint64_t now)
{
    ++g_isrDepth;
    for (Task* task = g_allTasks; task != nullptr; task = task->allNext)
    {
        if (task->state != TaskState::Blocked || task->deadlineUs > now) continue;
        WaitRemove(task);
        task->deadlineUs = kForever;
        task->timedOut = true;
        MakeReady(task);
    }
    // A source may fire, re-arm, register or drop others; bounded so a source that re-arms in the
    // past cannot hold the scheduler.
    for (int round = 0; round < 64; ++round)
    {
        TimedSource* due = nullptr;
        for (TimedSource* source = g_sources; source != nullptr; source = source->next)
            if (source->due <= now && (due == nullptr || source->due < due->due)) due = source;
        if (due == nullptr) break;
        due->due = kForever;
        due->fire(due, now);
    }
    --g_isrDepth;
    g_nextEventDirty = true;
}

void Reap()
{
    while (g_dead != nullptr)
    {
        Task* task = g_dead;
        g_dead = task->allNext;
        ContextDestroy(&task->context);
        task->magic = 0;
        task->~Task();
        RawFree(task);
    }
}

void Unlink(Task* task)
{
    Task** link = &g_allTasks;
    while (*link != nullptr && *link != task) link = &(*link)->allNext;
    if (*link == task) *link = task->allNext;
    task->allNext = nullptr;
    --g_taskCount;
    for (Task*& last : g_lastOnCore)
        if (last == task) last = nullptr;
}

void RunTlsDeleters(Task* task)
{
    for (int index = 0; index < kTlsPointers; ++index)
        if (task->tlsDelete[index] != nullptr) task->tlsDelete[index](index, task->tls[index]);
}

// ---- switching --------------------------------------------------------------------------------

// From a task: back to the scheduler. Returns when the scheduler runs this task again.
void SwitchOut()
{
    Task* task = g_current;
    const uint64_t now = NowUs();
    task->runTimeUs += now - task->switchedInUs;
    g_current = nullptr;
    ContextSwitch(&task->context, &g_schedulerContext);
}

void TaskMain(void* arg)
{
    Task* task = static_cast<Task*>(arg);
    task->entry(task->arg);
    // ESP-IDF's port aborts when a task function returns.
    Fatal("Task %s should not return, aborting now!", task->name);
}

void Preempt(bool toBack)
{
    Task* task = g_current;
    MakeReady(task, !toBack);
    SwitchOut();
}

void RunTask(Task* task, int core)
{
    ReadyRemove(task);
    task->state = TaskState::Running;
    g_current = task;
    g_currentCore = core;
    g_lastOnCore[core] = task;
    const uint64_t start = NowUs();
    task->switchedInUs = start;
    task->sliceStartUs = start;
    ContextSwitch(&g_schedulerContext, &task->context);
    const uint64_t ran = NowUs() - start;
    g_coreTimeUs[core] += ran;
    g_busyUs += ran;
    if (ran >= g_hogWarningUs && !task->isService)
    {
        static Task* warned = nullptr;
        if (warned != task)
        {
            warned = task;
            std::fprintf(stderr,
                         "W (%llu) cardenza-web: task %s ran %llu ms without blocking; the other tasks and the page "
                         "waited (one thread runs them all: block with vTaskDelay, a queue or a semaphore instead "
                         "of busy-waiting)\n",
                         static_cast<unsigned long long>(NowUs() / 1000), task->name,
                         static_cast<unsigned long long>(ran / 1000));
        }
    }
}

}  // namespace

// ---- clock ------------------------------------------------------------------------------------

uint64_t NowUs()
{
    const uint64_t now = g_testClock != nullptr ? g_testClock() : PlatformNowUs();
    if (now > g_lastNowUs) g_lastNowUs = now;
    return g_lastNowUs;
}

uint32_t TickRateHz() { return g_tickRateHz; }
uint64_t TickUs() { return g_tickUs; }
uint32_t TickCount() { return static_cast<uint32_t>(NowUs() / g_tickUs); }

void SetClockForTest(uint64_t (*now)())
{
    g_testClock = now;
    g_lastNowUs = 0;
}

uint64_t DeadlineForTicks(uint32_t ticks)
{
    if (ticks == 0xFFFFFFFFu) return kForever;
    const uint64_t tick = NowUs() / g_tickUs;
    return (tick + ticks) * g_tickUs;
}

uint64_t DeadlineForUs(uint64_t micros)
{
    if (micros == kForever) return kForever;
    return NowUs() + micros;
}

// ---- lifecycle --------------------------------------------------------------------------------

void Init(uint32_t tickRateHz)
{
    if (g_started) return;
    g_tickRateHz = tickRateHz == 0 ? 1000 : tickRateHz;
    g_tickUs = 1000000u / g_tickRateHz;
    ContextInitMain(&g_schedulerContext);
    g_started = true;
}

bool Started() { return g_started; }

Task* CreateTask(void (*entry)(void*), void* arg, const char* name, uint32_t stackBytes, unsigned priority, int core,
                 bool service)
{
    if (entry == nullptr) return nullptr;
    if (!g_started) Init(g_tickRateHz);
    if (priority >= kMaxPriorities) priority = kMaxPriorities - 1;
    if (core != kNoAffinity && (core < 0 || core >= kCores)) return nullptr;
    void* memory = RawAlloc(sizeof(Task), alignof(Task));
    if (memory == nullptr) return nullptr;
    Task* task = new (memory) Task();
    task->magic = kTaskMagic;
    std::snprintf(task->name, sizeof(task->name), "%s", name != nullptr ? name : "");
    task->entry = entry;
    task->arg = arg;
    task->basePriority = task->priority = priority;
    task->core = core;
    task->stackBytes = stackBytes;
    task->number = ++g_taskNumber;
    task->isService = service;
    std::size_t contextStack = static_cast<std::size_t>(stackBytes) * 2;
    if (contextStack < kMinStackBytes) contextStack = kMinStackBytes;
    if (!ContextCreate(&task->context, TaskMain, task, contextStack, kAsyncBytes))
    {
        task->magic = 0;
        task->~Task();
        RawFree(memory);
        return nullptr;
    }
    task->allNext = g_allTasks;
    g_allTasks = task;
    ++g_taskCount;
    MakeReady(task);
    if (g_current != nullptr && task->priority > g_current->priority && Eligible(task, g_currentCore))
        g_yieldPending = true;
    return task;
}

void DeleteTask(Task* task)
{
    if (task == nullptr) task = g_current;
    if (task == nullptr || task->magic != kTaskMagic) return;
    if (task == g_current)
    {
        if (g_critical != 0 || g_suspendAll != 0 || g_isrDepth != 0)
            Fatal("vTaskDelete(NULL) inside a critical section or with the scheduler suspended");
        RunTlsDeleters(task);
        task->state = TaskState::Deleted;
        Unlink(task);
        task->allNext = g_dead;
        g_dead = task;
        SwitchOut();
        Fatal("a deleted task ran again");
    }
    switch (task->state)
    {
        case TaskState::Ready: ReadyRemove(task); break;
        case TaskState::Blocked: WaitRemove(task); break;
        default: break;
    }
    RunTlsDeleters(task);
    task->state = TaskState::Deleted;
    Unlink(task);
    task->allNext = g_dead;
    g_dead = task;
    g_nextEventDirty = true;
    if (g_current == nullptr) Reap();
}

Task* Current() { return g_isrDepth == 0 ? g_current : nullptr; }
int CurrentCore() { return g_currentCore; }
Task* LastOnCore(int core) { return core >= 0 && core < kCores ? g_lastOnCore[core] : nullptr; }
void SetIdleHook(void (*hook)()) { g_idleHook = hook; }
Task* AllTasks() { return g_allTasks; }
unsigned TaskCount() { return g_taskCount; }
bool ValidTask(const Task* task) { return task != nullptr && task->magic == kTaskMagic; }

// ---- context checks ---------------------------------------------------------------------------

bool InIsr() { return g_isrDepth != 0; }
bool InTask() { return g_current != nullptr && g_isrDepth == 0; }

void EnterCritical() { ++g_critical; }

void ExitCritical()
{
    if (g_critical > 0) --g_critical;
    if (g_critical == 0 && g_yieldPending && InTask()) PreemptionPoint();
}

bool InCritical() { return g_critical != 0; }

void SuspendAll() { ++g_suspendAll; }

bool ResumeAll()
{
    if (g_suspendAll > 0) --g_suspendAll;
    if (g_suspendAll == 0 && g_yieldPending && InTask() && g_critical == 0)
    {
        const Task* before = g_current;
        PreemptionPoint();
        return before != nullptr;
    }
    return false;
}

bool SchedulerSuspended() { return g_suspendAll != 0; }

// ---- blocking ---------------------------------------------------------------------------------

bool Block(WaitQueue* queue, uint64_t deadlineUs)
{
    Task* task = g_current;
    if (task == nullptr || g_isrDepth != 0) Fatal("a blocking call outside a task (in an ISR or the scheduler)");
    if (g_critical != 0) Fatal("task %s blocked inside a critical section", task->name);
    if (g_suspendAll != 0) Fatal("task %s blocked while the scheduler is suspended", task->name);
    task->state = TaskState::Blocked;
    task->deadlineUs = deadlineUs;
    task->timedOut = false;
    task->wakeValue = 0;
    if (queue != nullptr) WaitInsert(queue, task);
    if (deadlineUs < g_nextEventUs) g_nextEventUs = deadlineUs;
    SwitchOut();
    return !task->timedOut;
}

void SleepUntil(uint64_t deadlineUs)
{
    if (!InTask())
    {
        // No task to block (a constructor, the scheduler): wait the old way.
        while (NowUs() < deadlineUs) {}
        return;
    }
    if (deadlineUs <= NowUs())
    {
        Yield();
        return;
    }
    Block(nullptr, deadlineUs);
}

void Yield()
{
    if (!InTask() || g_critical != 0 || g_suspendAll != 0) return;
    g_yieldPending = false;
    Preempt(true);
}

void PreemptionPoint()
{
    Task* task = g_current;
    if (task == nullptr || g_isrDepth != 0 || g_critical != 0 || g_suspendAll != 0) return;
    const uint64_t now = NowUs();
    if (now >= NextEventUs()) ProcessEvents(now);
    g_yieldPending = false;
    const int core = g_currentCore;
    const int other = 1 - core;
    bool toBack = false;
    bool yield = false;
    if (Task* mine = PickFor(core, task->priority))
    {
        if (mine->priority > task->priority) yield = true;
        else if (now - task->sliceStartUs >= g_tickUs) yield = toBack = true;  // round robin among equals
    }
    if (!yield)
    {
        // The other core has work: on the device it would run now, beside this task.
        if (Task* theirs = PickFor(other))
        {
            const bool fresh = theirs->readySinceUs >= task->switchedInUs;  // woke while this task ran
            if (fresh || now - task->sliceStartUs >= g_tickUs || g_coreTimeUs[core] > g_coreTimeUs[other] + g_tickUs)
                yield = true;
        }
    }
    if (!yield && now - g_runEnteredUs >= g_runBudgetUs) yield = true;  // the page's turn
    if (yield) Preempt(toBack);
}

Task* WakeOne(WaitQueue* queue, uintptr_t value, int* higherPriorityWoken)
{
    Task* task = queue->head;
    if (task == nullptr) return nullptr;
    WakeTask(task, value, higherPriorityWoken);
    return task;
}

void WakeAll(WaitQueue* queue, uintptr_t value, int* higherPriorityWoken)
{
    while (queue->head != nullptr) WakeTask(queue->head, value, higherPriorityWoken);
}

void WakeTask(Task* task, uintptr_t value, int* higherPriorityWoken)
{
    if (task == nullptr || task->state != TaskState::Blocked) return;
    WaitRemove(task);
    task->deadlineUs = kForever;
    task->timedOut = false;
    task->wakeValue = value;
    MakeReady(task);
    g_nextEventDirty = true;
    const Task* running = g_current;
    const bool higher = running == nullptr || task->priority > running->priority || !Eligible(task, g_currentCore);
    if (higher)
    {
        if (g_isrDepth != 0)
        {
            if (higherPriorityWoken != nullptr) *higherPriorityWoken = 1;
        }
        else
        {
            g_yieldPending = true;
        }
    }
}

void SetPriority(Task* task, unsigned priority)
{
    if (priority >= kMaxPriorities) priority = kMaxPriorities - 1;
    if (task->priority == priority) return;
    if (task->state == TaskState::Ready)
    {
        ReadyRemove(task);
        task->priority = priority;
        ReadyInsert(task, false);
    }
    else if (task->state == TaskState::Blocked && task->waitingOn != nullptr)
    {
        WaitQueue* queue = task->waitingOn;
        WaitRemove(task);
        task->priority = priority;
        WaitInsert(queue, task);
    }
    else
    {
        task->priority = priority;
    }
    if (InTask()) g_yieldPending = true;
}

void SetBasePriority(Task* task, unsigned priority)
{
    if (priority >= kMaxPriorities) priority = kMaxPriorities - 1;
    const bool inherited = task->priority > task->basePriority;
    task->basePriority = priority;
    if (!inherited || priority > task->priority) SetPriority(task, priority);
}

void Suspend(Task* task)
{
    if (task == nullptr) task = g_current;
    if (task == nullptr) return;
    switch (task->state)
    {
        case TaskState::Ready:
            ReadyRemove(task);
            task->state = TaskState::Suspended;
            break;
        case TaskState::Blocked:
            WaitRemove(task);
            task->deadlineUs = kForever;
            task->timedOut = true;  // the blocking call returns as if it timed out
            task->state = TaskState::Suspended;
            g_nextEventDirty = true;
            break;
        case TaskState::Running:
            if (task == g_current && InTask())
            {
                if (g_critical != 0 || g_suspendAll != 0) Fatal("vTaskSuspend(NULL) with the scheduler suspended");
                task->state = TaskState::Suspended;
                SwitchOut();
            }
            break;
        default: break;
    }
}

bool Resume(Task* task)
{
    if (task == nullptr || task->state != TaskState::Suspended) return false;
    MakeReady(task);
    const bool higher = g_current == nullptr || task->priority > g_current->priority;
    if (higher && g_isrDepth == 0) g_yieldPending = true;
    return higher;
}

void YieldFromIsrRequest(int higherPriorityWoken)
{
    if (higherPriorityWoken && InTask()) g_yieldPending = true;
}

// ---- interrupt sources ------------------------------------------------------------------------

void Register(TimedSource* source)
{
    if (source->registered) return;
    source->next = g_sources;
    g_sources = source;
    source->registered = true;
    g_nextEventDirty = true;
}

void Unregister(TimedSource* source)
{
    if (!source->registered) return;
    TimedSource** link = &g_sources;
    while (*link != nullptr && *link != source) link = &(*link)->next;
    if (*link == source) *link = source->next;
    source->next = nullptr;
    source->registered = false;
    g_nextEventDirty = true;
}

void Reschedule(TimedSource* source, uint64_t dueUs)
{
    source->due = dueUs;
    if (dueUs < g_nextEventUs) g_nextEventUs = dueUs;
    else g_nextEventDirty = true;
}

void RunIsr(void (*fn)(void*), void* arg)
{
    ++g_isrDepth;
    fn(arg);
    --g_isrDepth;
}

// ---- the page's side --------------------------------------------------------------------------

void RunFor(uint64_t budgetUs)
{
    if (!g_started || g_current != nullptr) return;
    g_runEnteredUs = NowUs();
    g_runBudgetUs = budgetUs;
    for (;;)
    {
        Reap();
        const uint64_t now = NowUs();
        if (now >= NextEventUs()) ProcessEvents(now);
        Task* candidates[kCores] = {PickFor(0), PickFor(1)};
        if (candidates[0] == nullptr && candidates[1] == nullptr)
        {
            if (g_idleHook != nullptr)
            {
                ++g_isrDepth;
                g_idleHook();
                --g_isrDepth;
            }
            break;
        }
        // An idle core banks no time: it starts even with the busy one.
        for (int core = 0; core < kCores; ++core)
            if (candidates[core] == nullptr && g_coreTimeUs[core] < g_coreTimeUs[1 - core]) g_coreTimeUs[core] = g_coreTimeUs[1 - core];
        // Both cores have work: the one that ran less goes (on the device both would run at once).
        int core = 0;
        if (candidates[0] == nullptr) core = 1;
        else if (candidates[1] != nullptr && g_coreTimeUs[1] < g_coreTimeUs[0]) core = 1;
        RunTask(candidates[core], core);
        if (NowUs() - g_runEnteredUs >= budgetUs) break;
    }
    Reap();
}

int64_t NextDelayUs()
{
    if (!g_started) return -1;
    if (g_readyMask != 0) return 0;
    const uint64_t next = NextEventUs();
    if (next == kForever) return -1;
    const uint64_t now = NowUs();
    return next <= now ? 0 : static_cast<int64_t>(next - now);
}

uint64_t IdleUs()
{
    const uint64_t now = NowUs();
    return now > g_busyUs ? now - g_busyUs : 0;
}

void SetHogWarningUs(uint64_t micros) { g_hogWarningUs = micros; }

void Fatal(const char* format, ...)
{
    char message[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    std::fprintf(stderr, "E (%llu) cardenza-web: %s\n", static_cast<unsigned long long>(NowUs() / 1000), message);
    std::fflush(stderr);
    std::abort();
}

}  // namespace cardenza_web::kernel
