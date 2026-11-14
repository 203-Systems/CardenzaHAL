// GPIO and LEDC on the web target. Pins keep their configuration and level; GPIO0 reads the page's
// HOME key (active low) and raises its interrupt; GPIO33 resets the display controller; GPIO38 is the
// display backlight, as a plain output or an LEDC channel's duty.

#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/ledc.h"

#include "board_io.h"
#include "display_state.h"
#include "../kernel/kernel.h"

namespace k = cardenza_web::kernel;

namespace {

constexpr int kPins = GPIO_NUM_MAX;
constexpr int kFuncButton = 0;
constexpr int kDisplayReset = 33;
constexpr int kBacklight = 38;

struct Pin {
    gpio_mode_t mode = GPIO_MODE_DISABLE;
    bool pullUp = false;
    bool pullDown = false;
    bool level = false;
    gpio_int_type_t interrupt = GPIO_INTR_DISABLE;
    bool interruptEnabled = false;
    gpio_isr_t handler = nullptr;
    void* handlerArg = nullptr;
    int ledcChannel = -1;
};

Pin g_pins[kPins];
bool g_isrService = false;
bool g_home = false;
float g_backlight = 0.0f;

bool Valid(int pin) { return GPIO_IS_VALID_GPIO(pin); }

void BacklightFromPin()
{
    const Pin& pin = g_pins[kBacklight];
    if (pin.ledcChannel >= 0) return;
    if (pin.mode & GPIO_MODE_DEF_OUTPUT) cardenza_web::display::SetBacklight(pin.level ? 1.0f : 0.0f);
}

int Level(int pin)
{
    if (pin == kFuncButton) return g_home ? 0 : 1;
    const Pin& state = g_pins[pin];
    if (state.mode & GPIO_MODE_DEF_OUTPUT) return state.level ? 1 : 0;
    if (state.pullUp) return 1;
    return 0;
}

struct EdgeEvent {
    int pin;
    int before;
    int after;
};

void RaiseIsr(void* arg)
{
    const auto* event = static_cast<const EdgeEvent*>(arg);
    Pin& pin = g_pins[event->pin];
    if (!pin.interruptEnabled || pin.handler == nullptr) return;
    bool fire = false;
    switch (pin.interrupt)
    {
        case GPIO_INTR_POSEDGE: fire = event->before == 0 && event->after == 1; break;
        case GPIO_INTR_NEGEDGE: fire = event->before == 1 && event->after == 0; break;
        case GPIO_INTR_ANYEDGE: fire = event->before != event->after; break;
        case GPIO_INTR_LOW_LEVEL: fire = event->after == 0; break;
        case GPIO_INTR_HIGH_LEVEL: fire = event->after == 1; break;
        default: break;
    }
    if (fire) pin.handler(pin.handlerArg);
}

struct Ledc {
    int gpio = -1;
    ledc_timer_t timer = LEDC_TIMER_0;
    uint32_t duty = 0;
    uint32_t pendingDuty = 0;
    bool configured = false;
};
struct LedcTimer {
    uint32_t bits = 13;
    uint32_t frequency = 5000;
    bool configured = false;
    bool paused = false;
};
Ledc g_channels[LEDC_CHANNEL_MAX];
LedcTimer g_timers[LEDC_TIMER_MAX];

void ApplyChannel(int channel)
{
    const Ledc& ledc = g_channels[channel];
    if (ledc.gpio != kBacklight) return;
    const LedcTimer& timer = g_timers[ledc.timer];
    const float max = static_cast<float>((1u << timer.bits) - 1);
    cardenza_web::display::SetBacklight(timer.paused ? 0.0f : static_cast<float>(ledc.duty > max ? max : ledc.duty) / max);
}

}  // namespace

namespace cardenza_web::display {
void SetBacklight(float light) { g_backlight = light < 0 ? 0 : (light > 1 ? 1 : light); }
float Backlight() { return g_backlight; }
}  // namespace cardenza_web::display

namespace cardenza_web::board {

void SetHome(bool pressed)
{
    if (pressed == g_home) return;
    EdgeEvent event{kFuncButton, g_home ? 0 : 1, pressed ? 0 : 1};
    g_home = pressed;
    k::RunIsr(RaiseIsr, &event);
}

bool HomePressed() { return g_home; }

}  // namespace cardenza_web::board

extern "C" {

esp_err_t gpio_config(const gpio_config_t* config)
{
    if (config == nullptr || config->pin_bit_mask == 0) return ESP_ERR_INVALID_ARG;
    for (int pin = 0; pin < kPins; ++pin)
    {
        if ((config->pin_bit_mask & (1ULL << pin)) == 0) continue;
        if (!Valid(pin)) return ESP_ERR_INVALID_ARG;
        Pin& state = g_pins[pin];
        state.mode = config->mode;
        state.pullUp = config->pull_up_en == GPIO_PULLUP_ENABLE;
        state.pullDown = config->pull_down_en == GPIO_PULLDOWN_ENABLE;
        state.interrupt = config->intr_type;
        state.interruptEnabled = config->intr_type != GPIO_INTR_DISABLE;
        if (pin == kBacklight) BacklightFromPin();
    }
    return ESP_OK;
}

esp_err_t gpio_reset_pin(gpio_num_t gpio_num)
{
    if (!Valid(gpio_num)) return ESP_ERR_INVALID_ARG;
    g_pins[gpio_num] = Pin{};
    g_pins[gpio_num].pullUp = true;  // ESP-IDF: input disabled, output disabled, pull-up on
    return ESP_OK;
}

esp_err_t gpio_set_intr_type(gpio_num_t gpio_num, gpio_int_type_t intr_type)
{
    if (!Valid(gpio_num)) return ESP_ERR_INVALID_ARG;
    g_pins[gpio_num].interrupt = intr_type;
    return ESP_OK;
}

esp_err_t gpio_intr_enable(gpio_num_t gpio_num)
{
    if (!Valid(gpio_num)) return ESP_ERR_INVALID_ARG;
    g_pins[gpio_num].interruptEnabled = true;
    return ESP_OK;
}

esp_err_t gpio_intr_disable(gpio_num_t gpio_num)
{
    if (!Valid(gpio_num)) return ESP_ERR_INVALID_ARG;
    g_pins[gpio_num].interruptEnabled = false;
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    if (!Valid(gpio_num)) return ESP_ERR_INVALID_ARG;
    g_pins[gpio_num].level = level != 0;
    if (gpio_num == kDisplayReset) cardenza_web::display::ResetLine(level != 0);
    if (gpio_num == kBacklight) BacklightFromPin();
    return ESP_OK;
}

int gpio_get_level(gpio_num_t gpio_num)
{
    k::PreemptionPoint();
    return Valid(gpio_num) ? Level(gpio_num) : 0;
}

esp_err_t gpio_set_direction(gpio_num_t gpio_num, gpio_mode_t mode)
{
    if (!Valid(gpio_num)) return ESP_ERR_INVALID_ARG;
    g_pins[gpio_num].mode = mode;
    if (gpio_num == kBacklight) BacklightFromPin();
    return ESP_OK;
}

esp_err_t gpio_set_pull_mode(gpio_num_t gpio_num, gpio_pull_mode_t pull)
{
    if (!Valid(gpio_num)) return ESP_ERR_INVALID_ARG;
    g_pins[gpio_num].pullUp = pull == GPIO_PULLUP_ONLY || pull == GPIO_PULLUP_PULLDOWN;
    g_pins[gpio_num].pullDown = pull == GPIO_PULLDOWN_ONLY || pull == GPIO_PULLUP_PULLDOWN;
    return ESP_OK;
}

esp_err_t gpio_wakeup_enable(gpio_num_t gpio_num, gpio_int_type_t) { return Valid(gpio_num) ? ESP_OK : ESP_ERR_INVALID_ARG; }
esp_err_t gpio_wakeup_disable(gpio_num_t gpio_num) { return Valid(gpio_num) ? ESP_OK : ESP_ERR_INVALID_ARG; }
esp_err_t gpio_isr_register(void (*)(void*), void*, int, gpio_isr_handle_t*) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t gpio_pullup_en(gpio_num_t gpio_num) { return gpio_set_pull_mode(gpio_num, GPIO_PULLUP_ONLY); }

esp_err_t gpio_pullup_dis(gpio_num_t gpio_num)
{
    if (!Valid(gpio_num)) return ESP_ERR_INVALID_ARG;
    g_pins[gpio_num].pullUp = false;
    return ESP_OK;
}

esp_err_t gpio_pulldown_en(gpio_num_t gpio_num)
{
    if (!Valid(gpio_num)) return ESP_ERR_INVALID_ARG;
    g_pins[gpio_num].pullDown = true;
    return ESP_OK;
}

esp_err_t gpio_pulldown_dis(gpio_num_t gpio_num)
{
    if (!Valid(gpio_num)) return ESP_ERR_INVALID_ARG;
    g_pins[gpio_num].pullDown = false;
    return ESP_OK;
}

esp_err_t gpio_install_isr_service(int)
{
    if (g_isrService) return ESP_ERR_INVALID_STATE;
    g_isrService = true;
    return ESP_OK;
}

void gpio_uninstall_isr_service(void) { g_isrService = false; }

esp_err_t gpio_isr_handler_add(gpio_num_t gpio_num, gpio_isr_t isr_handler, void* args)
{
    if (!g_isrService) return ESP_ERR_INVALID_STATE;
    if (!Valid(gpio_num)) return ESP_ERR_INVALID_ARG;
    g_pins[gpio_num].handler = isr_handler;
    g_pins[gpio_num].handlerArg = args;
    return ESP_OK;
}

esp_err_t gpio_isr_handler_remove(gpio_num_t gpio_num)
{
    if (!Valid(gpio_num)) return ESP_ERR_INVALID_ARG;
    g_pins[gpio_num].handler = nullptr;
    return ESP_OK;
}

esp_err_t gpio_set_drive_capability(gpio_num_t gpio_num, gpio_drive_cap_t) { return Valid(gpio_num) ? ESP_OK : ESP_ERR_INVALID_ARG; }

esp_err_t gpio_get_drive_capability(gpio_num_t gpio_num, gpio_drive_cap_t* strength)
{
    if (!Valid(gpio_num) || strength == nullptr) return ESP_ERR_INVALID_ARG;
    *strength = GPIO_DRIVE_CAP_DEFAULT;
    return ESP_OK;
}

esp_err_t gpio_hold_en(gpio_num_t gpio_num) { return Valid(gpio_num) ? ESP_OK : ESP_ERR_INVALID_ARG; }
esp_err_t gpio_hold_dis(gpio_num_t gpio_num) { return Valid(gpio_num) ? ESP_OK : ESP_ERR_INVALID_ARG; }
void gpio_deep_sleep_hold_en(void) {}
void gpio_deep_sleep_hold_dis(void) {}
void gpio_iomux_in(uint32_t, uint32_t) {}
void gpio_iomux_out(uint8_t, int, bool) {}

esp_err_t gpio_dump_io_configuration(FILE* out, uint64_t mask)
{
    for (int pin = 0; pin < kPins; ++pin)
        if (mask & (1ULL << pin))
            std::fprintf(out, "IO[%d] mode %d pullup %d pulldown %d level %d\n", pin, g_pins[pin].mode, g_pins[pin].pullUp,
                         g_pins[pin].pullDown, Level(pin));
    return ESP_OK;
}

// ---- LEDC -----------------------------------------------------------------------------------------

esp_err_t ledc_timer_config(const ledc_timer_config_t* config)
{
    if (config == nullptr || config->timer_num >= LEDC_TIMER_MAX || config->speed_mode >= LEDC_SPEED_MODE_MAX) return ESP_ERR_INVALID_ARG;
    LedcTimer& timer = g_timers[config->timer_num];
    if (config->deconfigure)
    {
        timer = LedcTimer{};
        return ESP_OK;
    }
    if (config->duty_resolution < LEDC_TIMER_1_BIT || config->duty_resolution >= LEDC_TIMER_BIT_MAX || config->freq_hz == 0)
        return ESP_ERR_INVALID_ARG;
    timer.bits = config->duty_resolution;
    timer.frequency = config->freq_hz;
    timer.configured = true;
    return ESP_OK;
}

esp_err_t ledc_channel_config(const ledc_channel_config_t* config)
{
    if (config == nullptr || config->channel >= LEDC_CHANNEL_MAX || config->timer_sel >= LEDC_TIMER_MAX || !Valid(config->gpio_num))
        return ESP_ERR_INVALID_ARG;
    Ledc& ledc = g_channels[config->channel];
    if (ledc.gpio >= 0 && ledc.gpio < kPins) g_pins[ledc.gpio].ledcChannel = -1;
    ledc.gpio = config->gpio_num;
    ledc.timer = config->timer_sel;
    ledc.duty = ledc.pendingDuty = config->duty;
    ledc.configured = true;
    g_pins[config->gpio_num].ledcChannel = config->channel;
    g_pins[config->gpio_num].mode = GPIO_MODE_OUTPUT;
    ApplyChannel(config->channel);
    return ESP_OK;
}

uint32_t ledc_find_suitable_duty_resolution(uint32_t src_clk_freq, uint32_t timer_freq)
{
    if (timer_freq == 0) return 0;
    uint32_t ratio = src_clk_freq / timer_freq;
    uint32_t bits = 0;
    while (ratio > 1 && bits < 14)
    {
        ratio >>= 1;
        ++bits;
    }
    return bits;
}

esp_err_t ledc_update_duty(ledc_mode_t, ledc_channel_t channel)
{
    if (channel >= LEDC_CHANNEL_MAX || !g_channels[channel].configured) return ESP_ERR_INVALID_STATE;
    g_channels[channel].duty = g_channels[channel].pendingDuty;
    ApplyChannel(channel);
    return ESP_OK;
}

esp_err_t ledc_set_pin(int gpio_num, ledc_mode_t, ledc_channel_t channel)
{
    if (channel >= LEDC_CHANNEL_MAX || !Valid(gpio_num)) return ESP_ERR_INVALID_ARG;
    g_channels[channel].gpio = gpio_num;
    g_pins[gpio_num].ledcChannel = channel;
    ApplyChannel(channel);
    return ESP_OK;
}

esp_err_t ledc_stop(ledc_mode_t, ledc_channel_t channel, uint32_t idle_level)
{
    if (channel >= LEDC_CHANNEL_MAX) return ESP_ERR_INVALID_ARG;
    Ledc& ledc = g_channels[channel];
    if (ledc.gpio == kBacklight) cardenza_web::display::SetBacklight(idle_level ? 1.0f : 0.0f);
    return ESP_OK;
}

esp_err_t ledc_set_freq(ledc_mode_t, ledc_timer_t timer_num, uint32_t freq_hz)
{
    if (timer_num >= LEDC_TIMER_MAX || freq_hz == 0) return ESP_ERR_INVALID_ARG;
    g_timers[timer_num].frequency = freq_hz;
    return ESP_OK;
}

uint32_t ledc_get_freq(ledc_mode_t, ledc_timer_t timer_num) { return timer_num < LEDC_TIMER_MAX ? g_timers[timer_num].frequency : 0; }

esp_err_t ledc_set_duty_with_hpoint(ledc_mode_t mode, ledc_channel_t channel, uint32_t duty, uint32_t)
{
    return ledc_set_duty(mode, channel, duty);
}

int ledc_get_hpoint(ledc_mode_t, ledc_channel_t) { return 0; }

esp_err_t ledc_set_duty(ledc_mode_t, ledc_channel_t channel, uint32_t duty)
{
    if (channel >= LEDC_CHANNEL_MAX) return ESP_ERR_INVALID_ARG;
    g_channels[channel].pendingDuty = duty;
    return ESP_OK;
}

uint32_t ledc_get_duty(ledc_mode_t, ledc_channel_t channel) { return channel < LEDC_CHANNEL_MAX ? g_channels[channel].duty : LEDC_ERR_DUTY; }

esp_err_t ledc_timer_rst(ledc_mode_t, ledc_timer_t timer_sel) { return timer_sel < LEDC_TIMER_MAX ? ESP_OK : ESP_ERR_INVALID_ARG; }

esp_err_t ledc_timer_pause(ledc_mode_t, ledc_timer_t timer_sel)
{
    if (timer_sel >= LEDC_TIMER_MAX) return ESP_ERR_INVALID_ARG;
    g_timers[timer_sel].paused = true;
    for (int channel = 0; channel < LEDC_CHANNEL_MAX; ++channel)
        if (g_channels[channel].timer == timer_sel) ApplyChannel(channel);
    return ESP_OK;
}

esp_err_t ledc_timer_resume(ledc_mode_t, ledc_timer_t timer_sel)
{
    if (timer_sel >= LEDC_TIMER_MAX) return ESP_ERR_INVALID_ARG;
    g_timers[timer_sel].paused = false;
    for (int channel = 0; channel < LEDC_CHANNEL_MAX; ++channel)
        if (g_channels[channel].timer == timer_sel) ApplyChannel(channel);
    return ESP_OK;
}

esp_err_t ledc_bind_channel_timer(ledc_mode_t, ledc_channel_t channel, ledc_timer_t timer_sel)
{
    if (channel >= LEDC_CHANNEL_MAX || timer_sel >= LEDC_TIMER_MAX) return ESP_ERR_INVALID_ARG;
    g_channels[channel].timer = timer_sel;
    ApplyChannel(channel);
    return ESP_OK;
}

// Fades reach their target at once.
esp_err_t ledc_set_fade_with_time(ledc_mode_t mode, ledc_channel_t channel, uint32_t target_duty, int)
{
    return ledc_set_duty(mode, channel, target_duty);
}

esp_err_t ledc_set_fade_with_step(ledc_mode_t mode, ledc_channel_t channel, uint32_t target_duty, uint32_t, uint32_t)
{
    return ledc_set_duty(mode, channel, target_duty);
}

esp_err_t ledc_fade_func_install(int) { return ESP_OK; }
void ledc_fade_func_uninstall(void) {}
esp_err_t ledc_fade_start(ledc_mode_t mode, ledc_channel_t channel, ledc_fade_mode_t) { return ledc_update_duty(mode, channel); }
esp_err_t ledc_fade_stop(ledc_mode_t, ledc_channel_t) { return ESP_OK; }

esp_err_t ledc_set_duty_and_update(ledc_mode_t mode, ledc_channel_t channel, uint32_t duty, uint32_t)
{
    ledc_set_duty(mode, channel, duty);
    return ledc_update_duty(mode, channel);
}

esp_err_t ledc_set_fade_time_and_start(ledc_mode_t mode, ledc_channel_t channel, uint32_t target_duty, uint32_t, ledc_fade_mode_t)
{
    ledc_set_duty(mode, channel, target_duty);
    return ledc_update_duty(mode, channel);
}

esp_err_t ledc_cb_register(ledc_mode_t, ledc_channel_t, ledc_cbs_t*, void*) { return ESP_OK; }

}  // extern "C"
