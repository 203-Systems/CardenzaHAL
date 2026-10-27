#include "keyboard_driver.h"
#include "keyboard_hardware.h"
#include "led_brightness.h"

#include <cstring>

#include <freertos/FreeRTOS.h>
#include <hal/spi_ll.h>

namespace CardenzaHAL::Keyboard {

void KeyboardDriver::_commit_led_frame()
{
    // Batch callers build and publish once for the whole frame. Build outside
    // the critical section; only the short copy blocks scanning.
    const auto masks = _led_frame.BuildScanMasks();
    static_assert(sizeof(masks) == sizeof(_scan_led_col_mask));
    portENTER_CRITICAL(&detail::kKeyboardStateMux);
    memcpy(_scan_led_col_mask, masks.data(), sizeof(_scan_led_col_mask));
    _scan_led_enabled = _led_enabled;
    portEXIT_CRITICAL(&detail::kKeyboardStateMux);
}

void KeyboardDriver::_shift_led_columns(uint8_t value)
{
    auto* hw = detail::gKeyboardLedSpiHw;
    while (spi_ll_get_running_cmd(hw) != 0) {}
    spi_ll_write_buffer_byte(hw, 0, &value, 1);
    spi_ll_set_mosi_bitlen(hw, 8);
    spi_ll_user_start(hw);
    while (spi_ll_get_running_cmd(hw) != 0) {}
}

bool KeyboardDriver::enableLED(bool enabled)
{
    if (enabled == _led_enabled) return _led_enabled;
    _led_enabled = enabled;
    _commit_led_frame();
    if (!enabled)
    {
        detail::GateKeyboardLeds(false);
        _scan_led_shifted_columns = 0xFF;
    }
    return _led_enabled;
}

void KeyboardDriver::setLED(Point2D_t pos, uint8_t brightness)
{
    if (_led_frame.SetKey(pos, brightness) && _led_enabled)
    {
        _commit_led_frame();
    }
}

void KeyboardDriver::setLEDFrame(const LedFrame& levels)
{
    if (_led_frame.Assign(levels) && _led_enabled)
    {
        _commit_led_frame();
    }
}

void KeyboardDriver::clearAllLEDs()
{
    if (_led_frame.Fill(0) && _led_enabled)
    {
        _commit_led_frame();
    }
}

void KeyboardDriver::setAllLEDs(uint8_t brightness)
{
    if (_led_frame.Fill(brightness) && _led_enabled)
    {
        _commit_led_frame();
    }
}

void KeyboardDriver::setLEDGlobalBrightness(uint8_t level)
{
    if (level > LED_BRIGHTNESS_MAX)
    {
        level = LED_BRIGHTNESS_MAX;
    }
    _global_brightness = level;
    detail::SetKeyboardLedGlobalDuty(detail::LedGlobalDutyForLevel(level));
}

}  // namespace CardenzaHAL::Keyboard
