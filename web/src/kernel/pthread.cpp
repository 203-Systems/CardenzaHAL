// POSIX threads on the web target's kernel, as ESP-IDF builds them on FreeRTOS: a pthread is a task
// (esp_pthread_set_cfg chooses its stack, priority, core and name), and mutexes, condition variables,
// read-write locks and sleeps block the calling task. std::thread, std::mutex, std::condition_variable
// and std::this_thread::sleep_for all come here: the web build wraps these calls (-Wl,--wrap=...),
// because Emscripten's single-threaded C library would otherwise make every mutex a no-op.

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include "esp_pthread.h"

#include "kernel.h"
#include "raw_alloc.h"

namespace k = cardenza_web::kernel;

namespace cardenza_web::freertos {
void EnsureStarted();
}

namespace {

constexpr uint32_t kThreadMagic = 0x50544852;
constexpr uint32_t kCondMagic = 0x434F4E44;
constexpr uint32_t kRwMagic = 0x52574C4B;
constexpr int kMaxKeys = 64;
// The owner of a lock taken outside any task (a constructor before the scheduler starts).
k::Task* const kNoTask = reinterpret_cast<k::Task*>(static_cast<uintptr_t>(1));

struct Thread {
    uint32_t magic;
    k::Task* task;
    void* (*start)(void*);
    void* arg;
    void* result;
    bool done;
    bool detached;
    k::WaitQueue joiners;
};

struct Mutex {
    uint32_t type;  // PTHREAD_MUTEX_NORMAL / RECURSIVE / ERRORCHECK; zero-initialized = normal
    k::Task* owner;
    uint32_t count;
    k::WaitQueue waiters;
};
static_assert(sizeof(Mutex) <= sizeof(pthread_mutex_t), "pthread_mutex_t must hold the web target's mutex");

struct Cond {
    uint32_t magic;
    clockid_t clock;
    k::WaitQueue waiters;
};
static_assert(sizeof(Cond) <= sizeof(pthread_cond_t), "pthread_cond_t must hold the web target's condition");

struct RwLock {
    uint32_t magic;
    k::Task* writer;
    uint32_t readers;
    k::WaitQueue waiters;
};
static_assert(sizeof(RwLock) <= sizeof(pthread_rwlock_t), "pthread_rwlock_t must hold the web target's lock");

struct KeySlot {
    bool used;
    void (*destructor)(void*);
};
KeySlot g_keys[kMaxKeys];
void* g_noTaskValues[kMaxKeys];

esp_pthread_cfg_t g_config = {CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT, CONFIG_PTHREAD_TASK_PRIO_DEFAULT, false,
                              CONFIG_PTHREAD_TASK_NAME_DEFAULT, tskNO_AFFINITY, 0};
bool g_configSet = false;

k::Task* Me()
{
    k::Task* task = k::Current();
    return task != nullptr ? task : kNoTask;
}

void** Values(bool create)
{
    k::Task* task = k::Current();
    if (task == nullptr) return g_noTaskValues;
    if (task->pthreadKeys == nullptr && create)
    {
        task->pthreadKeys = cardenza_web::RawAlloc(sizeof(void*) * kMaxKeys);
        if (task->pthreadKeys != nullptr) std::memset(task->pthreadKeys, 0, sizeof(void*) * kMaxKeys);
    }
    return static_cast<void**>(task->pthreadKeys);
}

void RunKeyDestructors()
{
    void** values = Values(false);
    if (values == nullptr) return;
    for (int round = 0; round < 4; ++round)
    {
        bool any = false;
        for (int key = 0; key < kMaxKeys; ++key)
        {
            if (!g_keys[key].used || g_keys[key].destructor == nullptr || values[key] == nullptr) continue;
            void* value = values[key];
            values[key] = nullptr;
            g_keys[key].destructor(value);
            any = true;
        }
        if (!any) break;
    }
}

void FreeKeys(k::Task* task)
{
    cardenza_web::RawFree(task->pthreadKeys);
    task->pthreadKeys = nullptr;
}

[[noreturn]] void Finish(Thread* thread, void* result)
{
    RunKeyDestructors();
    k::Task* task = k::Current();
    FreeKeys(task);
    thread->result = result;
    thread->done = true;
    k::WakeAll(&thread->joiners);
    task->pthread = nullptr;
    if (thread->detached)
    {
        thread->magic = 0;
        cardenza_web::RawFree(thread);
    }
    k::DeleteTask(task);
    __builtin_unreachable();
}

void ThreadEntry(void* arg)
{
    auto* thread = static_cast<Thread*>(arg);
    Finish(thread, thread->start(thread->arg));
}

uint64_t DeadlineFrom(clockid_t clock, const struct timespec* when)
{
    struct timespec now;
    clock_gettime(clock, &now);
    const int64_t micros = (static_cast<int64_t>(when->tv_sec) - now.tv_sec) * 1000000 + (when->tv_nsec - now.tv_nsec) / 1000;
    return micros <= 0 ? k::NowUs() : k::NowUs() + static_cast<uint64_t>(micros);
}

int LockMutex(Mutex* mutex, uint64_t deadline, bool tryOnly)
{
    k::Task* me = Me();
    for (;;)
    {
        if (mutex->owner == nullptr)
        {
            mutex->owner = me;
            mutex->count = 1;
            return 0;
        }
        if (mutex->owner == me)
        {
            if (mutex->type == PTHREAD_MUTEX_RECURSIVE)
            {
                ++mutex->count;
                return 0;
            }
            if (mutex->type == PTHREAD_MUTEX_ERRORCHECK) return EDEADLK;
            if (tryOnly) return EBUSY;
            k::Fatal("task %s locked a pthread mutex it already holds (it would wait forever)", me != kNoTask ? me->name : "(none)");
        }
        if (tryOnly) return EBUSY;
        if (!k::InTask()) return EBUSY;  // nothing to wait with: no task runs
        if (!k::Block(&mutex->waiters, deadline) && mutex->owner != nullptr) return ETIMEDOUT;
    }
}

int UnlockMutex(Mutex* mutex)
{
    if (mutex->owner == nullptr) return EPERM;
    if (mutex->owner != Me() && mutex->type != PTHREAD_MUTEX_NORMAL) return EPERM;
    if (mutex->count > 1)
    {
        --mutex->count;
        return 0;
    }
    mutex->owner = nullptr;
    mutex->count = 0;
    k::WakeOne(&mutex->waiters);
    k::PreemptionPoint();
    return 0;
}

int CondWait(Cond* cond, Mutex* mutex, uint64_t deadline)
{
    if (mutex->owner != Me()) return EPERM;
    if (!k::InTask())
    {
        // No task to block: let time pass and say so, the caller loops on its predicate.
        return ETIMEDOUT;
    }
    const uint32_t count = mutex->count;
    mutex->count = 1;
    UnlockMutex(mutex);
    const bool woken = k::Block(&cond->waiters, deadline);
    LockMutex(mutex, k::kForever, false);
    mutex->count = count;
    return woken ? 0 : ETIMEDOUT;
}

Cond* AsCond(pthread_cond_t* cond)
{
    auto* c = reinterpret_cast<Cond*>(cond);
    if (c->magic != kCondMagic)
    {
        c->magic = kCondMagic;
        c->clock = CLOCK_REALTIME;
        c->waiters = {};
    }
    return c;
}

RwLock* AsRw(pthread_rwlock_t* lock)
{
    auto* rw = reinterpret_cast<RwLock*>(lock);
    if (rw->magic != kRwMagic) *rw = RwLock{kRwMagic, nullptr, 0, {}};
    return rw;
}

}  // namespace

extern "C" {

// ---- threads ----------------------------------------------------------------------------------------

int __wrap_pthread_create(pthread_t* out, const pthread_attr_t* attr, void* (*start)(void*), void* arg)
{
    cardenza_web::freertos::EnsureStarted();
    std::size_t stack = g_config.stack_size;
    int detach = PTHREAD_CREATE_JOINABLE;
    if (attr != nullptr)
    {
        std::size_t asked = 0;
        if (pthread_attr_getstacksize(attr, &asked) == 0 && asked != 0) stack = asked;
        pthread_attr_getdetachstate(attr, &detach);
    }
    auto* thread = static_cast<Thread*>(cardenza_web::RawAlloc(sizeof(Thread)));
    if (thread == nullptr) return EAGAIN;
    new (thread) Thread{kThreadMagic, nullptr, start, arg, nullptr, false, detach == PTHREAD_CREATE_DETACHED, {}};
    const int core = g_config.pin_to_core == tskNO_AFFINITY ? k::kNoAffinity : g_config.pin_to_core;
    k::Task* task = k::CreateTask(ThreadEntry, thread, g_config.thread_name != nullptr ? g_config.thread_name : "pthread",
                                  static_cast<uint32_t>(stack), static_cast<unsigned>(g_config.prio), core);
    if (task == nullptr)
    {
        cardenza_web::RawFree(thread);
        return EAGAIN;
    }
    thread->task = task;
    task->pthread = thread;
    if (!g_config.inherit_cfg && g_configSet)
    {
        // ESP-IDF applies a configuration to the next thread only unless it asks to be inherited.
        g_config = esp_pthread_get_default_config();
        g_configSet = false;
    }
    *out = reinterpret_cast<pthread_t>(thread);
    k::PreemptionPoint();
    return 0;
}

int __wrap_pthread_join(pthread_t id, void** result)
{
    auto* thread = reinterpret_cast<Thread*>(id);
    if (thread == nullptr || thread->magic != kThreadMagic) return ESRCH;
    if (thread->detached) return EINVAL;
    if (thread->task == k::Current() && !thread->done) return EDEADLK;
    while (!thread->done)
    {
        if (!k::InTask()) return EDEADLK;
        k::Block(&thread->joiners, k::kForever);
    }
    if (result != nullptr) *result = thread->result;
    thread->magic = 0;
    cardenza_web::RawFree(thread);
    return 0;
}

int __wrap_pthread_detach(pthread_t id)
{
    auto* thread = reinterpret_cast<Thread*>(id);
    if (thread == nullptr || thread->magic != kThreadMagic) return ESRCH;
    if (thread->done)
    {
        thread->magic = 0;
        cardenza_web::RawFree(thread);
        return 0;
    }
    thread->detached = true;
    return 0;
}

void __wrap_pthread_exit(void* result)
{
    k::Task* task = k::Current();
    if (task != nullptr && task->pthread != nullptr) Finish(static_cast<Thread*>(task->pthread), result);
    // A FreeRTOS task (not a pthread) ending itself this way.
    if (task != nullptr) k::DeleteTask(task);
    std::abort();
}

pthread_t __real_pthread_self(void);

pthread_t __wrap_pthread_self(void)
{
    k::Task* task = k::Current();
    if (task == nullptr) return __real_pthread_self();
    if (task->pthread != nullptr) return reinterpret_cast<pthread_t>(task->pthread);
    return reinterpret_cast<pthread_t>(task);
}

int __wrap_sched_yield(void)
{
    k::Yield();
    return 0;
}

// ---- sleeping -----------------------------------------------------------------------------------------

int __real_nanosleep(const struct timespec*, struct timespec*);

int __wrap_nanosleep(const struct timespec* request, struct timespec* remaining)
{
    if (request == nullptr || request->tv_nsec < 0 || request->tv_nsec >= 1000000000) return errno = EINVAL, -1;
    if (!k::InTask()) return __real_nanosleep(request, remaining);
    const uint64_t micros = static_cast<uint64_t>(request->tv_sec) * 1000000 + static_cast<uint64_t>(request->tv_nsec + 999) / 1000;
    k::SleepUntil(k::NowUs() + micros);
    if (remaining != nullptr) *remaining = {0, 0};
    return 0;
}

int __wrap_clock_nanosleep(clockid_t clock, int flags, const struct timespec* request, struct timespec* remaining)
{
    if (request == nullptr) return EINVAL;
    struct timespec relative = *request;
    if (flags & TIMER_ABSTIME)
    {
        struct timespec now;
        clock_gettime(clock, &now);
        int64_t nanos = (static_cast<int64_t>(request->tv_sec) - now.tv_sec) * 1000000000 + (request->tv_nsec - now.tv_nsec);
        if (nanos < 0) nanos = 0;
        relative.tv_sec = static_cast<time_t>(nanos / 1000000000);
        relative.tv_nsec = static_cast<long>(nanos % 1000000000);
    }
    return __wrap_nanosleep(&relative, remaining) == 0 ? 0 : errno;
}

int __wrap_usleep(useconds_t micros)
{
    const struct timespec request = {static_cast<time_t>(micros / 1000000), static_cast<long>(micros % 1000000) * 1000};
    return __wrap_nanosleep(&request, nullptr);
}

unsigned __wrap_sleep(unsigned seconds)
{
    const struct timespec request = {static_cast<time_t>(seconds), 0};
    __wrap_nanosleep(&request, nullptr);
    return 0;
}

// ---- mutexes ------------------------------------------------------------------------------------------

int __wrap_pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr)
{
    int type = PTHREAD_MUTEX_NORMAL;
    if (attr != nullptr) pthread_mutexattr_gettype(attr, &type);
    std::memset(mutex, 0, sizeof(*mutex));
    reinterpret_cast<Mutex*>(mutex)->type = static_cast<uint32_t>(type);
    return 0;
}

int __wrap_pthread_mutex_destroy(pthread_mutex_t* mutex)
{
    return reinterpret_cast<Mutex*>(mutex)->owner != nullptr ? EBUSY : 0;
}

int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) { return LockMutex(reinterpret_cast<Mutex*>(mutex), k::kForever, false); }
int __wrap_pthread_mutex_trylock(pthread_mutex_t* mutex) { return LockMutex(reinterpret_cast<Mutex*>(mutex), k::kForever, true); }

int __wrap_pthread_mutex_timedlock(pthread_mutex_t* mutex, const struct timespec* when)
{
    return LockMutex(reinterpret_cast<Mutex*>(mutex), DeadlineFrom(CLOCK_REALTIME, when), false);
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t* mutex) { return UnlockMutex(reinterpret_cast<Mutex*>(mutex)); }

// ---- condition variables ------------------------------------------------------------------------------

int __wrap_pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr)
{
    std::memset(cond, 0, sizeof(*cond));
    Cond* c = AsCond(cond);
    if (attr != nullptr) pthread_condattr_getclock(attr, &c->clock);
    return 0;
}

int __wrap_pthread_cond_destroy(pthread_cond_t* cond)
{
    return AsCond(cond)->waiters.Empty() ? 0 : EBUSY;
}

int __wrap_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex)
{
    return CondWait(AsCond(cond), reinterpret_cast<Mutex*>(mutex), k::kForever);
}

int __wrap_pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* when)
{
    Cond* c = AsCond(cond);
    return CondWait(c, reinterpret_cast<Mutex*>(mutex), DeadlineFrom(c->clock, when));
}

int __wrap_pthread_cond_signal(pthread_cond_t* cond)
{
    k::WakeOne(&AsCond(cond)->waiters);
    k::PreemptionPoint();
    return 0;
}

int __wrap_pthread_cond_broadcast(pthread_cond_t* cond)
{
    k::WakeAll(&AsCond(cond)->waiters);
    k::PreemptionPoint();
    return 0;
}

// ---- read-write locks ---------------------------------------------------------------------------------

int __wrap_pthread_rwlock_init(pthread_rwlock_t* lock, const pthread_rwlockattr_t*)
{
    std::memset(lock, 0, sizeof(*lock));
    AsRw(lock);
    return 0;
}

int __wrap_pthread_rwlock_destroy(pthread_rwlock_t*) { return 0; }

int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t* lock)
{
    RwLock* rw = AsRw(lock);
    while (rw->writer != nullptr)
    {
        if (!k::InTask()) return EBUSY;
        k::Block(&rw->waiters, k::kForever);
    }
    ++rw->readers;
    return 0;
}

int __wrap_pthread_rwlock_tryrdlock(pthread_rwlock_t* lock)
{
    RwLock* rw = AsRw(lock);
    if (rw->writer != nullptr) return EBUSY;
    ++rw->readers;
    return 0;
}

int __wrap_pthread_rwlock_wrlock(pthread_rwlock_t* lock)
{
    RwLock* rw = AsRw(lock);
    while (rw->writer != nullptr || rw->readers != 0)
    {
        if (!k::InTask()) return EBUSY;
        k::Block(&rw->waiters, k::kForever);
    }
    rw->writer = Me();
    return 0;
}

int __wrap_pthread_rwlock_trywrlock(pthread_rwlock_t* lock)
{
    RwLock* rw = AsRw(lock);
    if (rw->writer != nullptr || rw->readers != 0) return EBUSY;
    rw->writer = Me();
    return 0;
}

int __wrap_pthread_rwlock_unlock(pthread_rwlock_t* lock)
{
    RwLock* rw = AsRw(lock);
    if (rw->writer != nullptr) rw->writer = nullptr;
    else if (rw->readers > 0) --rw->readers;
    else return EPERM;
    k::WakeAll(&rw->waiters);
    k::PreemptionPoint();
    return 0;
}

// ---- once and keys ------------------------------------------------------------------------------------

int __wrap_pthread_once(pthread_once_t* once, void (*init)(void))
{
    // 0: never, 1: running (another task may be in it), 2: done.
    while (*once == 1) k::SleepUntil(k::NowUs() + 1000);
    if (*once == 2) return 0;
    *once = 1;
    init();
    *once = 2;
    return 0;
}

int __wrap_pthread_key_create(pthread_key_t* key, void (*destructor)(void*))
{
    for (int index = 0; index < kMaxKeys; ++index)
    {
        if (g_keys[index].used) continue;
        g_keys[index] = {true, destructor};
        *key = static_cast<pthread_key_t>(index);
        return 0;
    }
    return EAGAIN;
}

int __wrap_pthread_key_delete(pthread_key_t key)
{
    if (key >= static_cast<pthread_key_t>(kMaxKeys) || !g_keys[key].used) return EINVAL;
    g_keys[key] = {false, nullptr};
    return 0;
}

void* __wrap_pthread_getspecific(pthread_key_t key)
{
    if (key >= static_cast<pthread_key_t>(kMaxKeys)) return nullptr;
    void** values = Values(false);
    return values != nullptr ? values[key] : nullptr;
}

int __wrap_pthread_setspecific(pthread_key_t key, const void* value)
{
    if (key >= static_cast<pthread_key_t>(kMaxKeys) || !g_keys[key].used) return EINVAL;
    void** values = Values(true);
    if (values == nullptr) return ENOMEM;
    values[key] = const_cast<void*>(value);
    return 0;
}

// ---- ESP-IDF's pthread configuration ------------------------------------------------------------------

esp_pthread_cfg_t esp_pthread_get_default_config(void)
{
    return {CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT, CONFIG_PTHREAD_TASK_PRIO_DEFAULT, false, CONFIG_PTHREAD_TASK_NAME_DEFAULT,
            tskNO_AFFINITY, 0};
}

esp_err_t esp_pthread_set_cfg(const esp_pthread_cfg_t* cfg)
{
    if (cfg == nullptr || cfg->stack_size < PTHREAD_STACK_MIN) return ESP_ERR_INVALID_ARG;
    g_config = *cfg;
    if (g_config.prio == 0 || g_config.prio >= configMAX_PRIORITIES) g_config.prio = CONFIG_PTHREAD_TASK_PRIO_DEFAULT;
    g_configSet = true;
    return ESP_OK;
}

esp_err_t esp_pthread_get_cfg(esp_pthread_cfg_t* p)
{
    if (p == nullptr) return ESP_ERR_INVALID_ARG;
    if (!g_configSet) return ESP_ERR_NOT_FOUND;
    *p = g_config;
    return ESP_OK;
}

esp_err_t esp_pthread_init(void) { return ESP_OK; }

}  // extern "C"
