#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "font5x7.h"
#include "i18n.h"
#include "text.h"
#include "text_ink.h"

// The 203 Launcher's Lcd (src/platform/lcd.h there) as far as the system menu draws with it: an 8-bit grey
// canvas with a clip, and the Launcher's own drawing functions (lcd_draw.inc, copied by
// tools/launcher_ui.py). The HAL has no frame of its own to spare, so the canvas is one band of rows at a
// time (band()): rows outside it are clipped like pixels outside the screen, and the menu is drawn once
// for each band.
namespace CardenzaHAL::SystemMenu::launcher {

constexpr int SCREEN_WIDTH = 240, SCREEN_HEIGHT = 135;

class Lcd {
  public:
    struct Clip {
        int x, y, w, h;
    };
    // Rows [top, top + rows) of the screen, `pixels` holding SCREEN_WIDTH * rows of them. Clears the clip.
    void band(uint8_t* pixels, int top, int rows)
    {
        pixels_ = Pixels{pixels, top};
        bandTop_ = std::clamp(top, 0, SCREEN_HEIGHT);
        bandBottom_ = std::clamp(top + rows, bandTop_, SCREEN_HEIGHT);
        clearClip();
    }
    int width() const { return SCREEN_WIDTH; }
    int height() const { return SCREEN_HEIGHT; }
    void clip(int x, int y, int w, int h)
    {
        x0_ = std::clamp(x, 0, width());
        y0_ = std::clamp(y, bandTop_, bandBottom_);
        x1_ = std::clamp(x + w, x0_, width());
        y1_ = std::clamp(y + h, y0_, bandBottom_);
    }
    void clearClip() { clip(0, 0, width(), height()); }
    Clip getClip() const { return {x0_, y0_, x1_ - x0_, y1_ - y0_}; }
    void intersectClip(int x, int y, int w, int h)
    {
        int left = std::max(x, x0_), top = std::max(y, y0_);
        clip(left, top, std::max(0, std::min(x + w, x1_) - left), std::max(0, std::min(y + h, y1_) - top));
    }
    void greyRect(int x, int y, int w, int h, uint8_t grey);
    void text(const String& s, int x, int y, int scale, uint8_t grey, uint8_t opacity = 255);
    void textVertical(const String& s, int x, int y, uint8_t grey, int scale = 1);
    // The Launcher's grey to RGB565 (src/platform/lcd.h there).
    static uint16_t rgb(uint8_t g) { return (uint16_t(g >> 3) << 11) | (uint16_t(g >> 2) << 5) | (g >> 3); }

  private:
    // The Launcher indexes its whole canvas (pixels_ + row * width() + x); here that index lands in the band.
    struct Pixels {
        uint8_t* base = nullptr;
        int top = 0;
        uint8_t* operator+(int offset) const { return base + (offset - top * SCREEN_WIDTH); }
        uint8_t& operator[](int offset) const { return base[offset - top * SCREEN_WIDTH]; }
        bool operator!() const { return base == nullptr; }
    };
    Pixels pixels_;
    int bandTop_ = 0, bandBottom_ = 0;
    int x0_ = 0, y0_ = 0, x1_ = 0, y1_ = 0;
};

}  // namespace CardenzaHAL::SystemMenu::launcher
