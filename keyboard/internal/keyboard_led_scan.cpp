#include "keyboard_driver.h"
#include "keyboard_hardware.h"
#include "led_brightness.h"

#include <cstring>

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <hal/spi_ll.h>

namespace CardenzaHAL::Keyboard {

namespace {

// The lowest master level while the system menu is open, so its keys show even when the firmware dimmed
// the lights to nothing.
constexpr uint8_t kOverlayGlobalLevel = 8;

}  // namespace

void KeyboardDriver::_commit_led_frame()
{
    // Batch callers build and publish once for the whole frame. Build outside
    // the critical section; only the short copy blocks scanning. The firmware's
    // task and the system menu's both commit: one at a time (under the key
    // snapshot's mutex, which the scan task holds only for a copy).
    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    const bool overlay = _overlay.load();
    const auto masks = (overlay ? _overlay_frame : _led_frame).BuildScanMasks();
    static_assert(sizeof(masks) == sizeof(_scan_led_col_mask));
    portENTER_CRITICAL(&detail::kKeyboardStateMux);
    memcpy(_scan_led_col_mask, masks.data(), sizeof(_scan_led_col_mask));
    _scan_led_enabled = overlay || _led_enabled;
    portEXIT_CRITICAL(&detail::kKeyboardStateMux);
    if (_mutex) xSemaphoreGive(_mutex);
}

void KeyboardDriver::beginOverlay(const LedFrame& lights)
{
    if (!_initialized) return;
    (void)_overlay_frame.Assign(lights);
    _overlay.store(true);
    _commit_led_frame();
    if (_global_brightness < kOverlayGlobalLevel)
        detail::SetKeyboardLedGlobalDuty(detail::LedGlobalDutyForLevel(kOverlayGlobalLevel));
}

void KeyboardDriver::lightsOffForSleep()
{
    if (!_initialized) return;
    // No column mask: the scan never opens the LED enable again; then it is a GPIO held high (off).
    (void)_overlay_frame.Fill(0);
    _overlay.store(true);
    _commit_led_frame();
    detail::GateKeyboardLeds(false);
    (void)gpio_hold_en(static_cast<gpio_num_t>(led_pin_list[0]));
}

void KeyboardDriver::endOverlay()
{
    if (!_overlay.load()) return;
    _hide_held.store(true);
    _overlay.store(false);
    _commit_led_frame();
    if (!_led_enabled)
    {
        detail::GateKeyboardLeds(false);
        _scan_led_shifted_columns = 0xFF;
    }
    detail::SetKeyboardLedGlobalDuty(detail::LedGlobalDutyForLevel(_global_brightness));
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
    if (!enabled && !_overlay.load())
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
    if (_overlay.load() && level < kOverlayGlobalLevel) level = kOverlayGlobalLevel;
    detail::SetKeyboardLedGlobalDuty(detail::LedGlobalDutyForLevel(level));
}

}  // namespace CardenzaHAL::Keyboard
