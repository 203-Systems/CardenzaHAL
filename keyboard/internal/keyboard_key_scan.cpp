#include "keyboard_driver.h"
#include "keyboard_hardware.h"
#include "led_brightness.h"

#include <cstring>

#include <driver/gpio.h>
#include <driver/gptimer.h>
#include <esp_check.h>
#include <esp_ipc.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace CardenzaHAL::Keyboard {

void KeyboardDriver::_set_output(uint8_t output)
{
    const uint64_t set_mask =
        (uint64_t(output & 1u) << Pins::kKeyboardOutputs[0]) |
        (uint64_t((output >> 1) & 1u) << Pins::kKeyboardOutputs[1]) |
        (uint64_t((output >> 2) & 1u) << Pins::kKeyboardOutputs[2]);

    detail::FastWriteMask(set_mask, detail::KeyboardOutputMask() & ~set_mask);
}

uint8_t KeyboardDriver::_get_input()
{
    static_assert([] {
        for (const int pin : Pins::kKeyboardInputs) if (pin < 0 || pin >= 32) return false;
        return true;
    }(), "Keyboard inputs must use the low GPIO bank");
    // All seven active-low inputs belong to this bank. Read one coherent GPIO
    // snapshot instead of calling FastReadPin and reading the register seven times.
    const uint32_t pressed = ~GPIO.in;
    uint8_t buffer = 0;
    for (std::size_t i = 0; i < Pins::kKeyboardInputs.size(); ++i)
    {
        buffer |= static_cast<uint8_t>(((pressed >> Pins::kKeyboardInputs[i]) & 1u) << i);
    }

    return buffer;
}

bool KeyboardDriver::init()
{
    if (_initialized) return true;
    for (auto pin : led_pin_list)
    {
        if (gpio_reset_pin((gpio_num_t)pin) != ESP_OK ||
            gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT) != ESP_OK ||
            gpio_set_level((gpio_num_t)pin, 0) != ESP_OK) return false;
    }
    if (gpio_set_level((gpio_num_t)led_pin_list[0], 1) != ESP_OK) return false;
    // Prepare active-low OE before releasing Launcher's GPIO holds.
    for (auto pin : led_pin_list)
        if (gpio_hold_dis((gpio_num_t)pin) != ESP_OK) return false;

    detail::InitKeyboardLedSpi();
    if (detail::InitKeyboardLedPwm() != ESP_OK ||
        detail::SetKeyboardLedGlobalDuty(detail::LedGlobalDutyForLevel(_global_brightness)) != ESP_OK)
        return false;
    _shift_led_columns(0x00);
    _scan_led_shifted_columns = 0x00;
    for (auto pin : output_list)
    {
        if (gpio_reset_pin((gpio_num_t)pin) != ESP_OK ||
            gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT) != ESP_OK ||
            gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_PULLDOWN) != ESP_OK ||
            gpio_set_level((gpio_num_t)pin, 0) != ESP_OK) return false;
    }
    for (auto pin : input_list)
    {
        if (gpio_reset_pin((gpio_num_t)pin) != ESP_OK ||
            gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT) != ESP_OK ||
            gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY) != ESP_OK) return false;
    }
    _set_output(0);
    _mutex = xSemaphoreCreateMutex();
    if (_mutex == nullptr) return false;
    _key_list_buffer.reserve(LED_ROWS * LED_COLUMNS);

    gptimer_handle_t timer = nullptr;
    bool timerEnabled = false;
    const auto fail = [&]() {
        detail::GateKeyboardLeds(false);
        if (timer != nullptr)
        {
            if (timerEnabled) (void)gptimer_disable(timer);
            (void)gptimer_del_timer(timer);
        }
        if (_scan_task_handle != nullptr) vTaskDelete(_scan_task_handle);
        _scan_task_handle = nullptr;
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
        return false;
    };
    gptimer_config_t timerConfig = {};
    timerConfig.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    timerConfig.direction = GPTIMER_COUNT_UP;
    timerConfig.resolution_hz = detail::kLedTimerResolutionHz;
    timerConfig.intr_priority = detail::kScanInterruptPriority;
    if (gptimer_new_timer(&timerConfig, &timer) != ESP_OK) return fail();
    gptimer_event_callbacks_t callbacks = {};
    callbacks.on_alarm = detail::KeyboardScanTimerCallback;
#if portNUM_PROCESSORS > 1
    // The interrupt is allocated on the core that registers the callbacks (kScanInterruptCore).
    struct Registration
    {
        gptimer_handle_t timer;
        gptimer_event_callbacks_t* callbacks;
        void* user;
        esp_err_t result;
    } registration{timer, &callbacks, this, ESP_FAIL};
    const esp_err_t ipc = esp_ipc_call_blocking(detail::kScanInterruptCore, [](void* arg) {
        auto* r = static_cast<Registration*>(arg);
        r->result = gptimer_register_event_callbacks(r->timer, r->callbacks, r->user);
    }, &registration);
    if (ipc != ESP_OK || registration.result != ESP_OK) return fail();
#else
    if (gptimer_register_event_callbacks(timer, &callbacks, this) != ESP_OK) return fail();
#endif
    gptimer_alarm_config_t alarm = {};
    alarm.alarm_count = detail::kLedBitPlaneTicks;
    if (gptimer_set_alarm_action(timer, &alarm) != ESP_OK || gptimer_enable(timer) != ESP_OK)
        return fail();
    timerEnabled = true;
#if portNUM_PROCESSORS > 1
    const BaseType_t created = xTaskCreatePinnedToCore(_scan_task_entry, "kb_scan", 4096, this,
        detail::kScanTaskPriority, &_scan_task_handle, detail::kScanTaskPinnedCore);
#else
    const BaseType_t created = xTaskCreate(_scan_task_entry, "kb_scan", 4096, this,
        detail::kScanTaskPriority, &_scan_task_handle);
#endif
    if (created != pdPASS) return fail();
    _commit_led_frame();
    if (gptimer_start(timer) != ESP_OK) return fail();
    _initialized = true;
    return true;
}

Point2D_t KeyboardDriver::getKey()
{
    updateKeyList();
    if (_key_list_buffer.empty())
    {
        Point2D_t coor;
        coor.x = -1;
        coor.y = -1;
        return coor;
    }
    return _key_list_buffer[0];
}

uint8_t KeyboardDriver::getKeyNum(Point2D_t keyCoor)
{
    if ((keyCoor.x < 0) || (keyCoor.y < 0))
    {
        return 0;
    }

    return static_cast<uint8_t>((keyCoor.y * 14) + (keyCoor.x + 1));
}

void KeyboardDriver::updateKeyList()
{
    if (!_mutex) return;
    uint8_t snapshot[8];
    xSemaphoreTake(_mutex, portMAX_DELAY);
    memcpy(snapshot, _task_key_scan, sizeof(snapshot));
    xSemaphoreGive(_mutex);
    if (memcmp(snapshot, _key_list_scan, sizeof(snapshot)) == 0) return;
    memcpy(_key_list_scan, snapshot, sizeof(snapshot));

    _key_list_buffer.clear();
    for (int row = 0; row < 8; ++row)
    {
        for (int column = 0; column < 7; ++column)
        {
            if (!(snapshot[row] & (1u << column))) continue;
            _key_list_buffer.push_back({row > 3 ? X_map_chart[column].x_1 : X_map_chart[column].x_2,
                                       3 - (row > 3 ? row - 4 : row)});
        }
    }
}

bool KeyboardDriver::isKeyPressing(int keyNum)
{
    if (_key_list_buffer.size())
    {
        for (const auto& i : _key_list_buffer)
        {
            if (getKeyNum(i) == keyNum)
            {
                return true;
            }
        }
    }
    return false;
}

bool KeyboardDriver::isChanged()
{
    if (_last_key_size != _key_list_buffer.size())
    {
        _last_key_size = static_cast<uint8_t>(_key_list_buffer.size());
        return true;
    }
    return false;
}

void KeyboardDriver::_scan_task_entry(void* arg)
{
    static_cast<KeyboardDriver*>(arg)->_scan_task_loop();
}

bool KeyboardDriver::handleTimerTickFromIsr()
{
    BaseType_t high_task_woken = pdFALSE;

    detail::GateKeyboardLeds(false);

    const uint8_t row = _scan_row;
    const uint8_t input = _get_input();

    const uint8_t next_row = static_cast<uint8_t>((row + 1) & 0x07);
    _scan_row = next_row;
    if (next_row == 0)
    {
        _pwm_phase = static_cast<uint8_t>((_pwm_phase + 1) % detail::kLedPwmPhaseCount);
    }

    _set_output(next_row);
    portENTER_CRITICAL_ISR(&detail::kKeyboardStateMux);
    _scan_keys_changed |= _scan_raw_key_scan[row] != input;
    _scan_raw_key_scan[row] = input;
    const uint8_t next_col_mask = _scan_led_enabled ? _scan_led_col_mask[_pwm_phase][next_row] : 0;
    const bool notify = next_row == 0 &&
        (_scan_keys_changed || _debounce_pending.load(std::memory_order_relaxed));
    if (next_row == 0) _scan_keys_changed = false;
    portEXIT_CRITICAL_ISR(&detail::kKeyboardStateMux);
    // Keep sampling at the original rate. Wake the task only on raw changes or
    // while the 3 ms debounce interval is pending, never for a stable held chord.
    // A new edge always queues its own notification, even if the task is already
    // processing an older snapshot and clears _debounce_pending concurrently.
    if (notify) vTaskNotifyGiveFromISR(_scan_task_handle, &high_task_woken);
    if (next_col_mask)
    {
        if (next_col_mask != _scan_led_shifted_columns)
        {
            _shift_led_columns(next_col_mask);
            _scan_led_shifted_columns = next_col_mask;
        }
        detail::GateKeyboardLeds(true);
    }

    return high_task_woken == pdTRUE;
}

uint32_t KeyboardDriver::nextTimerTicks() const
{
    return detail::kLedBitPlaneTicks << _pwm_phase;
}

void KeyboardDriver::_scan_task_loop()
{
    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint8_t raw[8];
        portENTER_CRITICAL(&detail::kKeyboardStateMux);
        memcpy(raw, _scan_raw_key_scan, sizeof(raw));
        portEXIT_CRITICAL(&detail::kKeyboardStateMux);

        const bool changed = _debouncer.Update(raw, static_cast<uint32_t>(esp_timer_get_time()));
        _debounce_pending.store(_debouncer.Pending(), std::memory_order_relaxed);
        if (!changed) continue;

        xSemaphoreTake(_mutex, portMAX_DELAY);
        memcpy(_task_key_scan, _debouncer.Rows().data(), sizeof(_task_key_scan));
        xSemaphoreGive(_mutex);
    }
}

}  // namespace CardenzaHAL::Keyboard
