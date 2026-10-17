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

    bool SetBrightness(uint8_t brightness);
    uint8_t GetBrightness();
}
