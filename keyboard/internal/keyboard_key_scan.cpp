#include "keyboard_driver.h"
#include "keyboard_hardware.h"

#include <cstring>

#include <driver/gpio.h>
#include <driver/gptimer.h>
#include <esp_check.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace CardenzaHAL::Keyboard {

void KeyboardDriver::_set_output(uint8_t output)
{
    output = output & 0B00000111;

    uint64_t set_mask = 0;
    if (output & 0B00000001)
    {
        set_mask |= detail::PinMask(Pins::kKeyboardOutputs[0]);
    }
    if (output & 0B00000010)
    {
        set_mask |= detail::PinMask(Pins::kKeyboardOutputs[1]);
    }
    if (output & 0B00000100)
    {
        set_mask |= detail::PinMask(Pins::kKeyboardOutputs[2]);
    }

    detail::FastWriteMask(set_mask, detail::KeyboardOutputMask() & ~set_mask);
}

uint8_t KeyboardDriver::_get_input()
{
    uint8_t buffer = 0x00;
    uint8_t pin_value = 0x00;

    for (int i = 0; i < static_cast<int>(Pins::kKeyboardInputs.size()); i++)
    {
        pin_value = (detail::FastReadPin(Pins::kKeyboardInputs[i]) == 1) ? 0x00 : 0x01;
        pin_value = pin_value << i;
        buffer = buffer | pin_value;
    }

    return buffer;
}

void KeyboardDriver::init()
{
    for (auto i : led_pin_list)
    {
        gpio_reset_pin((gpio_num_t)i);
        gpio_set_direction((gpio_num_t)i, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)i, 0);
    }
    gpio_set_level((gpio_num_t)led_pin_list[0], 1);

    detail::InitKeyboardLedSpi();
    detail::InitKeyboardLedPwm();
    setLEDGlobalBrightness(_global_brightness);
    _shift_led_columns(0x00);
    _scan_led_shifted_columns = 0x00;

    for (auto i : output_list)
    {
        gpio_reset_pin((gpio_num_t)i);
        gpio_set_direction((gpio_num_t)i, GPIO_MODE_OUTPUT);
        gpio_set_pull_mode((gpio_num_t)i, GPIO_PULLUP_PULLDOWN);
        gpio_set_level((gpio_num_t)i, 0);
    }

    for (auto i : input_list)
    {
        gpio_reset_pin((gpio_num_t)i);
        gpio_set_direction((gpio_num_t)i, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)i, GPIO_PULLUP_ONLY);
    }

    _set_output(0);

    _mutex = xSemaphoreCreateMutex();

#if portNUM_PROCESSORS > 1
    xTaskCreatePinnedToCore(_scan_task_entry,
                            "kb_scan",
                            4096,
                            this,
                            detail::kScanTaskPriority,
                            &_scan_task_handle,
                            detail::kScanTaskPinnedCore);
#else
    xTaskCreate(_scan_task_entry, "kb_scan", 4096, this, detail::kScanTaskPriority, &_scan_task_handle);
#endif

    _commit_led_frame();

    gptimer_config_t timer_config = {};
    timer_config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    timer_config.direction = GPTIMER_COUNT_UP;
    timer_config.resolution_hz = detail::kLedTimerResolutionHz;
    gptimer_handle_t timer = nullptr;
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &timer));

    gptimer_event_callbacks_t callbacks = {};
    callbacks.on_alarm = detail::KeyboardScanTimerCallback;
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(timer, &callbacks, this));

    gptimer_alarm_config_t alarm_config = {};
    alarm_config.reload_count = 0;
    alarm_config.alarm_count = detail::kLedBitPlaneTicks;
    alarm_config.flags.auto_reload_on_alarm = false;
    ESP_ERROR_CHECK(gptimer_set_alarm_action(timer, &alarm_config));
    ESP_ERROR_CHECK(gptimer_enable(timer));
    ESP_ERROR_CHECK(gptimer_start(timer));

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
    if (_mutex)
    {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _key_list_buffer = _task_key_list;
        xSemaphoreGive(_mutex);
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
    _scan_raw_key_scan[row] = _get_input();

    const uint8_t next_row = static_cast<uint8_t>((row + 1) & 0x07);
    _scan_row = next_row;
    if (next_row == 0)
    {
        _pwm_phase = static_cast<uint8_t>((_pwm_phase + 1) % detail::kLedPwmPhaseCount);
        // Key scanning must not wait for a complete brightness cycle.
        vTaskNotifyGiveFromISR(_scan_task_handle, &high_task_woken);
    }

    _set_output(next_row);
    portENTER_CRITICAL_ISR(&detail::kKeyboardStateMux);
    const uint8_t next_col_mask = _scan_led_enabled ? _scan_led_col_mask[_pwm_phase][next_row] : 0;
    portEXIT_CRITICAL_ISR(&detail::kKeyboardStateMux);
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
    std::vector<Point2D_t> local_key_list;
    local_key_list.reserve(16);

    auto rebuild_debounced_key_list = [this, &local_key_list]() {
        const uint32_t now_us = static_cast<uint32_t>(esp_timer_get_time());
        constexpr uint32_t kDebounceUs = 3000;
        for (int row = 0; row < 8; ++row)
        {
            const uint8_t raw_value = _raw_key_scan[row];
            if (raw_value == _debounced_key_scan[row])
            {
                _candidate_key_scan[row] = raw_value;
                _debounce_since_us[row] = now_us;
                continue;
            }

            if (_candidate_key_scan[row] != raw_value)
            {
                _candidate_key_scan[row] = raw_value;
                _debounce_since_us[row] = now_us;
                continue;
            }

            // Weighted bit planes have unequal duration: count elapsed time,
            // not scan notifications, to preserve a stable debounce interval.
            if (static_cast<uint32_t>(now_us - _debounce_since_us[row]) >= kDebounceUs)
            {
                _debounced_key_scan[row] = raw_value;
                _debounce_since_us[row] = now_us;
            }
        }

        local_key_list.clear();
        for (int i = 0; i < 8; i++)
        {
            const uint8_t input_value = _debounced_key_scan[i];
            if (input_value)
            {
                Point2D_t coor;
                for (int j = 0; j < 7; j++)
                {
                    if (input_value & (0x01 << j))
                    {
                        coor.x = (i > 3) ? X_map_chart[j].x_1 : X_map_chart[j].x_2;
                        coor.y = (i > 3) ? (i - 4) : i;
                        coor.y = -coor.y + 3;
                        local_key_list.push_back(coor);
                    }
                }
            }
        }
    };

    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        portENTER_CRITICAL(&detail::kKeyboardStateMux);
        memcpy(_raw_key_scan, _scan_raw_key_scan, sizeof(_raw_key_scan));
        portEXIT_CRITICAL(&detail::kKeyboardStateMux);

        rebuild_debounced_key_list();

        xSemaphoreTake(_mutex, portMAX_DELAY);
        _task_key_list = local_key_list;
        xSemaphoreGive(_mutex);
    }
}

}  // namespace CardenzaHAL::Keyboard
