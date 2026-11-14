// esp_timer on the web target: one interrupt source for the earliest alarm; callbacks run in the
// "esp_timer" task (priority 22, like ESP-IDF) or, with ESP_TIMER_ISR, in ISR context.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_timer.h"
#include "esp_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "../kernel/kernel.h"

namespace k = cardenza_web::kernel;

namespace cardenza_web::freertos {
void EnsureStarted();
}

struct esp_timer {
    uint32_t magic;
    esp_timer_cb_t callback;
    void* arg;
    esp_timer_dispatch_t dispatch;
    const char* name;
    bool skipUnhandled;
    uint64_t alarmUs;   // k::kForever when not armed
    uint64_t periodUs;  // 0 for one-shot
    uint32_t fired;
    esp_timer* next;
};

namespace {

constexpr uint32_t kTimerMagic = 0x45535054;
esp_timer* g_timers = nullptr;
k::TimedSource g_source;
k::Task* g_task = nullptr;
k::WaitQueue g_taskWake;
bool g_taskDue = false;

bool Valid(const esp_timer* timer) { return timer != nullptr && timer->magic == kTimerMagic; }

void Rearm()
{
    uint64_t next = k::kForever;
    for (esp_timer* timer = g_timers; timer != nullptr; timer = timer->next)
        if (timer->dispatch == ESP_TIMER_ISR && timer->alarmUs < next) next = timer->alarmUs;
    for (esp_timer* timer = g_timers; timer != nullptr; timer = timer->next)
        if (timer->dispatch != ESP_TIMER_ISR && timer->alarmUs < next) next = timer->alarmUs;
    k::Reschedule(&g_source, next);
}

// Advances a timer past `now` once it fired; false when it was a one-shot.
void Advance(esp_timer* timer, uint64_t now)
{
    ++timer->fired;
    if (timer->periodUs == 0)
    {
        timer->alarmUs = k::kForever;
        return;
    }
    timer->alarmUs += timer->periodUs;
    if (timer->skipUnhandled && timer->alarmUs <= now)
        timer->alarmUs = now + timer->periodUs - (now - timer->alarmUs) % timer->periodUs;
}

void Fire(k::TimedSource*, uint64_t now)
{
    bool forTask = false;
    for (esp_timer* timer = g_timers; timer != nullptr; timer = timer->next)
    {
        if (timer->alarmUs > now) continue;
        if (timer->dispatch == ESP_TIMER_ISR)
        {
            Advance(timer, now);
            timer->callback(timer->arg);
        }
        else
        {
            forTask = true;
        }
    }
    if (forTask)
    {
        g_taskDue = true;
        k::WakeAll(&g_taskWake);
    }
    Rearm();
}

void TimerTask(void*)
{
    for (;;)
    {
        while (!g_taskDue) k::Block(&g_taskWake, k::kForever);
        g_taskDue = false;
        const uint64_t now = k::NowUs();
        // One callback at a time, earliest first; the list may change in a callback.
        for (;;)
        {
            esp_timer* due = nullptr;
            for (esp_timer* timer = g_timers; timer != nullptr; timer = timer->next)
                if (timer->dispatch != ESP_TIMER_ISR && timer->alarmUs <= now && (due == nullptr || timer->alarmUs < due->alarmUs)) due = timer;
            if (due == nullptr) break;
            Advance(due, now);
            Rearm();
            due->callback(due->arg);
        }
        Rearm();
    }
}

void EnsureService()
{
    if (g_task != nullptr) return;
    cardenza_web::freertos::EnsureStarted();
    g_source.fire = Fire;
    k::Register(&g_source);
    g_task = k::CreateTask(TimerTask, nullptr, "esp_timer", ESP_TASK_TIMER_STACK, ESP_TASK_TIMER_PRIO, 0, true);
}

esp_err_t Arm(esp_timer* timer, uint64_t timeoutUs, uint64_t periodUs)
{
    if (!Valid(timer)) return ESP_ERR_INVALID_ARG;
    if (timer->alarmUs != k::kForever) return ESP_ERR_INVALID_STATE;
    timer->periodUs = periodUs;
    timer->alarmUs = k::NowUs() + timeoutUs;
    Rearm();
    return ESP_OK;
}

}  // namespace

extern "C" {

esp_err_t esp_timer_early_init(void) { return ESP_OK; }

esp_err_t esp_timer_init(void)
{
    EnsureService();
    return ESP_OK;
}

esp_err_t esp_timer_deinit(void) { return ESP_OK; }

esp_err_t esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* out_handle)
{
    if (args == nullptr || args->callback == nullptr || out_handle == nullptr || args->dispatch_method >= ESP_TIMER_MAX)
        return ESP_ERR_INVALID_ARG;
    EnsureService();
    auto* timer = static_cast<esp_timer*>(std::calloc(1, sizeof(esp_timer)));
    if (timer == nullptr) return ESP_ERR_NO_MEM;
    timer->magic = kTimerMagic;
    timer->callback = args->callback;
    timer->arg = args->arg;
    timer->dispatch = args->dispatch_method;
    timer->name = args->name;
    timer->skipUnhandled = args->skip_unhandled_events;
    timer->alarmUs = k::kForever;
    timer->next = g_timers;
    g_timers = timer;
    *out_handle = timer;
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us) { return Arm(timer, timeout_us, 0); }

esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period)
{
    if (period == 0) return ESP_ERR_INVALID_ARG;
    return Arm(timer, period, period);
}

esp_err_t esp_timer_restart(esp_timer_handle_t timer, uint64_t timeout_us)
{
    if (!Valid(timer)) return ESP_ERR_INVALID_ARG;
    if (timer->alarmUs == k::kForever) return ESP_ERR_INVALID_STATE;
    const uint64_t period = timer->periodUs != 0 ? timeout_us : 0;
    timer->alarmUs = k::kForever;
    return Arm(timer, timeout_us, period);
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{
    if (!Valid(timer)) return ESP_ERR_INVALID_ARG;
    if (timer->alarmUs == k::kForever) return ESP_ERR_INVALID_STATE;
    timer->alarmUs = k::kForever;
    Rearm();
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{
    if (!Valid(timer)) return ESP_ERR_INVALID_ARG;
    if (timer->alarmUs != k::kForever) return ESP_ERR_INVALID_STATE;
    esp_timer** link = &g_timers;
    while (*link != nullptr && *link != timer) link = &(*link)->next;
    if (*link == timer) *link = timer->next;
    timer->magic = 0;
    std::free(timer);
    return ESP_OK;
}

int64_t esp_timer_get_time(void)
{
    k::PreemptionPoint();
    return static_cast<int64_t>(k::NowUs());
}

int64_t esp_timer_get_next_alarm(void)
{
    uint64_t next = k::kForever;
    for (esp_timer* timer = g_timers; timer != nullptr; timer = timer->next)
        if (timer->alarmUs < next) next = timer->alarmUs;
    return next == k::kForever ? INT64_MAX : static_cast<int64_t>(next);
}

int64_t esp_timer_get_next_alarm_for_wake_up(void) { return esp_timer_get_next_alarm(); }

esp_err_t esp_timer_get_period(esp_timer_handle_t timer, uint64_t* period)
{
    if (!Valid(timer) || period == nullptr) return ESP_ERR_INVALID_ARG;
    *period = timer->periodUs;
    return ESP_OK;
}

esp_err_t esp_timer_get_expiry_time(esp_timer_handle_t timer, uint64_t* expiry)
{
    if (!Valid(timer) || expiry == nullptr) return ESP_ERR_INVALID_ARG;
    if (timer->periodUs != 0) return ESP_ERR_NOT_SUPPORTED;
    if (timer->alarmUs == k::kForever) return ESP_ERR_INVALID_STATE;
    *expiry = timer->alarmUs;
    return ESP_OK;
}

esp_err_t esp_timer_dump(FILE* stream)
{
    std::fprintf(stream, "Timer stats:\nName                  Period      Alarm         Times_armed   Times_trigg   Times_skip    Cb_exec_time\n");
    for (esp_timer* timer = g_timers; timer != nullptr; timer = timer->next)
        std::fprintf(stream, "%-20s  %-10llu  %-12lld  %-12u\n", timer->name != nullptr ? timer->name : "timer",
                     static_cast<unsigned long long>(timer->periodUs),
                     timer->alarmUs == k::kForever ? 0LL : static_cast<long long>(timer->alarmUs), static_cast<unsigned>(timer->fired));
    return ESP_OK;
}

void esp_timer_isr_dispatch_need_yield(void) { k::YieldFromIsrRequest(1); }
bool esp_timer_is_active(esp_timer_handle_t timer) { return Valid(timer) && timer->alarmUs != k::kForever; }

}  // extern "C"
