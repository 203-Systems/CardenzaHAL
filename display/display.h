#pragma once

#include <cstdint>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

namespace CardenzaHAL::Display {
    constexpr int WIDTH = 240;
    constexpr int HEIGHT = 135;

    bool Init();
    esp_lcd_panel_handle_t GetPanel();
    esp_lcd_panel_io_handle_t GetPanelIO();

    // Perceived brightness 0..255 (0 off). Gamma 2.2 is applied by the HAL to
    // 12-bit hardware PWM; callers must not apply another gamma correction.
    bool SetBrightness(uint8_t brightness);
    // Returns the requested logical level, not the corrected PWM duty.
    uint8_t GetBrightness();
}
