#pragma once

// The web target's scheduler: FreeRTOS semantics on one thread.
//
// Every FreeRTOS task (and pthread, and the ESP-IDF service tasks) is a context of its own
// (context.h). Exactly one runs at a time. The page calls RunFor() from the browser's event loop;
// it runs the highest-priority ready task until that task blocks, yields or is preempted, picks the
// next, and returns when nothing is ready or its time budget is spent. NextDelayUs() tells the page
// when to call again.
//
// Preemption happens at preemption points: every call into FreeRTOS, ESP-IDF or the HAL (they call
// PreemptionPoint() or block). There the running task gives way to a task of higher priority that
// became ready, to an equal-priority task once its time slice (one tick) is over, to due timers and
// interrupts, and to the browser once the budget of this RunFor() is spent. A task that computes
// without calling anything keeps the CPU (it cannot be interrupted); one that calls into the kernel
// shares it like on the device. Never inside a critical section, an ISR or vTaskSuspendAll().
//
// Interrupts (timer alarms, DMA completions, input from the page) are TimedSources fired from the
// scheduler or at a preemption point, in ISR context: they may use the FromISR calls, never block.

#include <cstddef>
#include <cstdint>

#include "context.h"

namespace cardenza_web::kernel {

constexpr uint64_t kForever = UINT64_MAX;
constexpr unsigned kMaxPriorities = 25;          // configMAX_PRIORITIES
constexpr int kNoAffinity = 0x7FFFFFFF;           // tskNO_AFFINITY
constexpr int kNotifyEntries = 3;
constexpr int kTlsPointers = 2;

struct Task;

// Tasks blocked on something (a queue's receivers, a mutex...): highest priority first, FIFO among equals.
struct WaitQueue {
    Task* head = nullptr;
    bool Empty() const { return head == nullptr; }
};

enum class TaskState : uint8_t { Ready, Running, Blocked, Suspended, Deleted };

struct Task {
    uint32_t magic = 0;
    char name[16] = {};
    void (*entry)(void*) = nullptr;
    void* arg = nullptr;
    unsigned basePriority = 0;   // what the task asked for
    unsigned priority = 0;       // with priority inheritance from the mutexes it holds
    int core = kNoAffinity;
    uint32_t stackBytes = 0;     // what the firmware asked for (bytes, as in ESP-IDF)
    TaskState state = TaskState::Ready;
    bool suspendedWhileBlocked = false;

    Task* readyNext = nullptr;   // ready list of its priority
    Task* readyPrev = nullptr;
    Task* waitNext = nullptr;    // the WaitQueue it is blocked on
    WaitQueue* waitingOn = nullptr;
    uint64_t deadlineUs = kForever;
    bool timedOut = false;
    uintptr_t wakeValue = 0;     // what the waker handed over (event bits, ...)
    const void* waitData = nullptr;  // what the task waits for (an event group's bits), for wakers to read

    uint32_t notifyValue[kNotifyEntries] = {};
    uint8_t notifyState[kNotifyEntries] = {};   // 0 not waiting, 1 waiting, 2 received
    int notifyWaitIndex = -1;
    WaitQueue notifyWaiters;      // only the task itself ever waits here

    void* tls[kTlsPointers] = {};
    void (*tlsDelete[kTlsPointers])(int, void*) = {};
    void* pthread = nullptr;      // the pthread record when it is one (pthread.cpp)
    void* pthreadKeys = nullptr;  // its pthread_setspecific values (pthread.cpp)
    void* errnoSlot = nullptr;
    void* heapCharge = nullptr;   // the stack and TCB the device takes from internal RAM (heap.cpp)

    unsigned mutexesHeld = 0;
    uint64_t sliceStartUs = 0;
    uint64_t readySinceUs = 0;
    uint64_t runTimeUs = 0;
    uint64_t switchedInUs = 0;
    uint32_t number = 0;          // creation order (uxTaskNumber)
    bool isService = false;       // the web target's own (esp_timer, Tmr Svc): not "the firmware's"
    Context context;
    Task* allNext = nullptr;
};

// A source of interrupts: Fire() runs in ISR context once NowUs() >= due. The owner moves `due`
// with Reschedule (kForever: nothing pending).
struct TimedSource {
    uint64_t due = kForever;
    void (*fire)(TimedSource* source, uint64_t nowUs) = nullptr;
    TimedSource* next = nullptr;
    bool registered = false;
};

// Clock: microseconds since boot, never decreasing.
uint64_t NowUs();
uint32_t TickRateHz();
uint64_t TickUs();
uint32_t TickCount();
// Host tests drive time themselves.
void SetClockForTest(uint64_t (*now)());

// Lifecycle
void Init(uint32_t tickRateHz);
bool Started();
Task* CreateTask(void (*entry)(void*), void* arg, const char* name, uint32_t stackBytes, unsigned priority, int core,
                 bool service = false);
void DeleteTask(Task* task);  // the current task: does not return
Task* Current();              // null in the scheduler and in ISRs
int CurrentCore();            // the virtual core of the running task (0 or 1)
Task* LastOnCore(int core);
// Called when nothing is ready (FreeRTOS idle hooks); in the scheduler, may not block.
void SetIdleHook(void (*hook)());
Task* AllTasks();
unsigned TaskCount();
bool ValidTask(const Task* task);

// Context checks
bool InIsr();
bool InTask();                  // a task is running and may block
void EnterCritical();
void ExitCritical();
bool InCritical();
void SuspendAll();
bool ResumeAll();               // true if it switched
bool SchedulerSuspended();

// Blocking (task context only). Returns true when woken by an event, false on timeout.
bool Block(WaitQueue* queue, uint64_t deadlineUs);
void SleepUntil(uint64_t deadlineUs);
void Yield();                   // to the back of its priority, like taskYIELD
void PreemptionPoint();         // see the top of this file
// Wakes the first waiter; returns it (null when none). In a task, a woken task of higher priority
// runs at the next preemption point (the caller's API returns through one); in an ISR it sets
// *higherPriorityWoken.
Task* WakeOne(WaitQueue* queue, uintptr_t value = 0, int* higherPriorityWoken = nullptr);
void WakeAll(WaitQueue* queue, uintptr_t value = 0, int* higherPriorityWoken = nullptr);
void WakeTask(Task* task, uintptr_t value, int* higherPriorityWoken);
void SetPriority(Task* task, unsigned priority);          // effective (inheritance)
void SetBasePriority(Task* task, unsigned priority);
void Suspend(Task* task);
bool Resume(Task* task);
// After a ...FromISR call made from a task (allowed by FreeRTOS too): switch when it asked for it.
void YieldFromIsrRequest(int higherPriorityWoken);
// Deadline for a FreeRTOS block time in ticks (portMAX_DELAY: forever), aligned to tick boundaries.
uint64_t DeadlineForTicks(uint32_t ticks);
uint64_t DeadlineForUs(uint64_t micros);

// Interrupt sources
void Register(TimedSource* source);
void Unregister(TimedSource* source);
void Reschedule(TimedSource* source, uint64_t dueUs);
// Runs fn(arg) in ISR context now (input from the page, an emulated DMA completion).
void RunIsr(void (*fn)(void*), void* arg);

// The page's side
void RunFor(uint64_t budgetUs);
int64_t NextDelayUs();          // 0 runnable now, -1 nothing pending (wait for input)
uint64_t IdleUs();              // time no task ran (run-time statistics)

// Diagnostics
void Fatal(const char* format, ...) __attribute__((noreturn, format(printf, 1, 2)));
// A task that has not let others run for this long gets a warning once (a busy wait?).
void SetHogWarningUs(uint64_t micros);

}  // namespace cardenza_web::kernel
