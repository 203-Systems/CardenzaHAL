#include "keyboard_driver.h"
#include "keyboard_hardware.h"

#include <cstring>

#include <freertos/FreeRTOS.h>
#include <hal/spi_ll.h>

namespace CardenzaHAL::Keyboard {

namespace {

const uint64_t kKeyboardLedClockMask = detail::PinMask(led_pin_list[1]);
const uint64_t kKeyboardLedDataMask = detail::PinMask(led_pin_list[2]);

}  // namespace

void KeyboardDriver::_commit_led_frame()
{
    portENTER_CRITICAL(&detail::kKeyboardStateMux);
    memcpy(_scan_led_level, _led_level, sizeof(_led_level));
    _scan_global_brightness = _global_brightness;
    _scan_led_enabled = _led_enabled;
    for (uint8_t phase = 0; phase < detail::kLedPwmPhaseCount; ++phase)
    {
        for (uint8_t scan_index = 0; scan_index < 8; ++scan_index)
        {
            _scan_led_col_mask[phase][scan_index] = _scan_led_enabled ? _compute_led_col_mask(scan_index, phase) : 0;
        }
    }
    portEXIT_CRITICAL(&detail::kKeyboardStateMux);
}

void KeyboardDriver::_shift_led_columns(uint8_t value)
{
    if (detail::gKeyboardLedSpiReady && detail::gKeyboardLedSpiHw != nullptr)
    {
        while (spi_ll_get_running_cmd(detail::gKeyboardLedSpiHw) != 0)
        {
        }

        spi_ll_write_buffer_byte(detail::gKeyboardLedSpiHw, 0, &value, 1);
        spi_ll_set_mosi_bitlen(detail::gKeyboardLedSpiHw, 8);
        spi_ll_user_start(detail::gKeyboardLedSpiHw);

        while (spi_ll_get_running_cmd(detail::gKeyboardLedSpiHw) != 0)
        {
        }
        return;
    }

    for (int bit = 7; bit >= 0; --bit)
    {
        const bool bit_set = ((value >> bit) & 0x01) != 0;
        detail::FastWriteMask(bit_set ? kKeyboardLedDataMask : 0,
                              bit_set ? 0 : kKeyboardLedDataMask);
        detail::FastWriteMask(kKeyboardLedClockMask, 0);
        detail::FastWriteMask(0, kKeyboardLedClockMask);
    }
}

uint8_t KeyboardDriver::_compute_led_col_mask(uint8_t scan_index, uint8_t phase) const
{
    int y = 0;
    int x_base = 0;
    if (scan_index < 4)
    {
        y = 3 - scan_index;
        x_base = 1;
    }
    else
    {
        y = 7 - scan_index;
        x_base = 0;
    }

    uint8_t mask = 0;
    const uint8_t global_br = _scan_global_brightness;

    for (int col = 0; col < 7; col++)
    {
        const int x = x_base + col * 2;
        const uint8_t level = _scan_led_level[y][x];
        const uint8_t effective = (level < global_br) ? level : global_br;
        if (static_cast<uint16_t>(effective) * detail::kLedPwmPhaseCount > static_cast<uint16_t>(phase) * (LED_BRIGHTNESS_MAX + 1))
        {
            const int physical_bit = col + detail::kLedColumnBitOffset;
            if (physical_bit >= 0 && physical_bit < 8)
            {
                mask |= static_cast<uint8_t>(1u << physical_bit);
            }
        }
    }

    return mask;
}

bool KeyboardDriver::enableLED(bool enabled)
{
    _led_enabled = enabled;
    _commit_led_frame();
    if (!enabled)
    {
        detail::FastWritePin(led_pin_list[0], true);
        _scan_led_shifted_columns = 0xFF;
    }
    return _led_enabled;
}

void KeyboardDriver::setLED(Point2D_t pos, uint8_t brightness)
{
    if (pos.x < 0 || pos.x > 13 || pos.y < 0 || pos.y > 3)
    {
        return;
    }
    if (brightness > LED_BRIGHTNESS_MAX)
    {
        brightness = LED_BRIGHTNESS_MAX;
    }
    _led_level[pos.y][pos.x] = brightness;
    if (_led_enabled)
    {
        _commit_led_frame();
    }
}

void KeyboardDriver::clearAllLEDs()
{
    memset(_led_level, 0, sizeof(_led_level));
    if (_led_enabled)
    {
        _commit_led_frame();
    }
}

void KeyboardDriver::setAllLEDs(uint8_t brightness)
{
    if (brightness > LED_BRIGHTNESS_MAX)
    {
        brightness = LED_BRIGHTNESS_MAX;
    }
    memset(_led_level, brightness, sizeof(_led_level));
    if (_led_enabled)
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
    if (_led_enabled)
    {
        _commit_led_frame();
    }
}

}  // namespace CardenzaHAL::Keyboard
