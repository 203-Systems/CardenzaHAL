// General-purpose timers on the web target: a count derived from the kernel's clock, alarms as
// interrupt sources (on_alarm in ISR context), auto-reload keeping its phase.

#include <cstdlib>
#include <new>

#include "driver/gptimer.h"

#include "../kernel/kernel.h"

namespace k = cardenza_web::kernel;

struct gptimer_t {
    k::TimedSource source;  // first: the source's address is the timer's
    uint32_t resolution;
    gptimer_count_direction_t direction;
    bool enabled;
    bool running;
    uint64_t baseCount;   // the count at baseUs
    uint64_t baseUs;
    bool alarmSet;
    gptimer_alarm_config_t alarm;
    gptimer_alarm_cb_t onAlarm;
    void* context;
};

namespace {

constexpr int kTimers = 4;  // SOC_TIMER_GROUP_TOTAL_TIMERS
int g_timers = 0;

uint64_t CountAt(const gptimer_t* timer, uint64_t now)
{
    if (!timer->running) return timer->baseCount;
    const uint64_t ticks = (now - timer->baseUs) * timer->resolution / 1000000u;
    return timer->direction == GPTIMER_COUNT_UP ? timer->baseCount + ticks : timer->baseCount - ticks;
}

uint64_t TimeOfCount(const gptimer_t* timer, uint64_t count)
{
    const uint64_t delta = timer->direction == GPTIMER_COUNT_UP ? count - timer->baseCount : timer->baseCount - count;
    return timer->baseUs + (delta * 1000000u + timer->resolution - 1) / timer->resolution;
}

void Rearm(gptimer_t* timer)
{
    uint64_t due = k::kForever;
    if (timer->enabled && timer->running && timer->alarmSet && timer->onAlarm != nullptr)
    {
        const uint64_t now = k::NowUs();
        const uint64_t count = CountAt(timer, now);
        const bool ahead = timer->direction == GPTIMER_COUNT_UP ? timer->alarm.alarm_count > count : timer->alarm.alarm_count < count;
        due = ahead ? TimeOfCount(timer, timer->alarm.alarm_count) : now;
    }
    k::Reschedule(&timer->source, due);
}

void Fire(k::TimedSource* source, uint64_t now)
{
    auto* timer = reinterpret_cast<gptimer_t*>(source);
    if (!timer->enabled || !timer->running || !timer->alarmSet) return;
    const uint64_t alarm = timer->alarm.alarm_count;
    const uint64_t alarmUs = TimeOfCount(timer, alarm);
    if (timer->alarm.flags.auto_reload_on_alarm)
    {
        timer->baseCount = timer->alarm.reload_count;
        timer->baseUs = alarmUs < now ? alarmUs : now;  // the reload happened at the alarm
    }
    else
    {
        timer->alarmSet = false;  // a one-shot alarm until it is set again
    }
    const gptimer_alarm_event_data_t data{CountAt(timer, now), alarm};
    timer->onAlarm(timer, &data, timer->context);
    Rearm(timer);
}

}  // namespace

extern "C" {

esp_err_t gptimer_new_timer(const gptimer_config_t* config, gptimer_handle_t* ret_timer)
{
    if (config == nullptr || ret_timer == nullptr || config->resolution_hz == 0) return ESP_ERR_INVALID_ARG;
    if (g_timers >= kTimers) return ESP_ERR_NOT_FOUND;
    auto* timer = static_cast<gptimer_t*>(std::calloc(1, sizeof(gptimer_t)));
    if (timer == nullptr) return ESP_ERR_NO_MEM;
    new (&timer->source) k::TimedSource();
    timer->source.fire = Fire;
    timer->resolution = config->resolution_hz;
    timer->direction = config->direction;
    k::Register(&timer->source);
    ++g_timers;
    *ret_timer = timer;
    return ESP_OK;
}

esp_err_t gptimer_del_timer(gptimer_handle_t timer)
{
    if (timer == nullptr) return ESP_ERR_INVALID_ARG;
    if (timer->enabled) return ESP_ERR_INVALID_STATE;
    k::Unregister(&timer->source);
    std::free(timer);
    --g_timers;
    return ESP_OK;
}

esp_err_t gptimer_set_raw_count(gptimer_handle_t timer, uint64_t value)
{
    if (timer == nullptr) return ESP_ERR_INVALID_ARG;
    timer->baseCount = value;
    timer->baseUs = k::NowUs();
    Rearm(timer);
    return ESP_OK;
}

esp_err_t gptimer_get_raw_count(gptimer_handle_t timer, uint64_t* value)
{
    if (timer == nullptr || value == nullptr) return ESP_ERR_INVALID_ARG;
    *value = CountAt(timer, k::NowUs());
    return ESP_OK;
}

esp_err_t gptimer_get_resolution(gptimer_handle_t timer, uint32_t* out_resolution)
{
    if (timer == nullptr || out_resolution == nullptr) return ESP_ERR_INVALID_ARG;
    *out_resolution = timer->resolution;
    return ESP_OK;
}

esp_err_t gptimer_get_captured_count(gptimer_handle_t timer, uint64_t* value) { return gptimer_get_raw_count(timer, value); }

esp_err_t gptimer_register_event_callbacks(gptimer_handle_t timer, const gptimer_event_callbacks_t* cbs, void* user_data)
{
    if (timer == nullptr || cbs == nullptr) return ESP_ERR_INVALID_ARG;
    if (timer->enabled) return ESP_ERR_INVALID_STATE;
    timer->onAlarm = cbs->on_alarm;
    timer->context = user_data;
    return ESP_OK;
}

esp_err_t gptimer_set_alarm_action(gptimer_handle_t timer, const gptimer_alarm_config_t* config)
{
    if (timer == nullptr) return ESP_ERR_INVALID_ARG;
    timer->alarmSet = config != nullptr;
    if (config != nullptr) timer->alarm = *config;
    // From the alarm callback this is re-armed by Fire().
    if (!k::InIsr()) Rearm(timer);
    return ESP_OK;
}

esp_err_t gptimer_enable(gptimer_handle_t timer)
{
    if (timer == nullptr) return ESP_ERR_INVALID_ARG;
    if (timer->enabled) return ESP_ERR_INVALID_STATE;
    timer->enabled = true;
    Rearm(timer);
    return ESP_OK;
}

esp_err_t gptimer_disable(gptimer_handle_t timer)
{
    if (timer == nullptr) return ESP_ERR_INVALID_ARG;
    if (!timer->enabled || timer->running) return ESP_ERR_INVALID_STATE;
    timer->enabled = false;
    Rearm(timer);
    return ESP_OK;
}

esp_err_t gptimer_start(gptimer_handle_t timer)
{
    if (timer == nullptr) return ESP_ERR_INVALID_ARG;
    if (!timer->enabled || timer->running) return ESP_ERR_INVALID_STATE;
    timer->baseUs = k::NowUs();
    timer->running = true;
    Rearm(timer);
    return ESP_OK;
}

esp_err_t gptimer_stop(gptimer_handle_t timer)
{
    if (timer == nullptr) return ESP_ERR_INVALID_ARG;
    if (!timer->running) return ESP_ERR_INVALID_STATE;
    timer->baseCount = CountAt(timer, k::NowUs());
    timer->running = false;
    Rearm(timer);
    return ESP_OK;
}

}  // extern "C"
