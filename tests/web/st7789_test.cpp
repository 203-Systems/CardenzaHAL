// The display controller model (web/src/drivers/st7789.h): the HAL's own setup shows the framebuffer
// upright; byte order, colour inversion, the window's wrap, display off and BGR behave as the panel.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "st7789.h"

using cardenza_web::display::St7789;

namespace {

constexpr int kWidth = St7789::kViewWidth;
constexpr int kHeight = St7789::kViewHeight;

void Command(St7789& panel, uint8_t command, std::vector<uint8_t> parameters = {})
{
    panel.Command(command, parameters.data(), parameters.size());
}

// What display/display.cpp sends through esp_lcd's ST7789 driver: SLPOUT, MADCTL, COLMOD, RAMCTRL
// (little endian), INVON, swap_xy + mirror_x, DISPON.
void HalSetup(St7789& panel)
{
    panel.Reset();
    Command(panel, 0x11);
    Command(panel, 0x36, {0x00});
    Command(panel, 0x3A, {0x55});
    Command(panel, 0xB0, {0x00, 0xF8});
    Command(panel, 0x21);
    Command(panel, 0x36, {0x20});
    Command(panel, 0x36, {0x60});
    Command(panel, 0x29);
}

// esp_lcd_panel_draw_bitmap with the HAL's gap (40, 53).
void Draw(St7789& panel, int x0, int y0, int x1, int y1, const std::vector<uint16_t>& pixels, bool little = true)
{
    const int xs = x0 + 40, xe = x1 + 40 - 1, ys = y0 + 53, ye = y1 + 53 - 1;
    Command(panel, 0x2A, {uint8_t(xs >> 8), uint8_t(xs), uint8_t(xe >> 8), uint8_t(xe)});
    Command(panel, 0x2B, {uint8_t(ys >> 8), uint8_t(ys), uint8_t(ye >> 8), uint8_t(ye)});
    Command(panel, 0x2C);
    std::vector<uint8_t> bytes;
    for (uint16_t pixel : pixels)
    {
        if (little)
        {
            bytes.push_back(uint8_t(pixel));
            bytes.push_back(uint8_t(pixel >> 8));
        }
        else
        {
            bytes.push_back(uint8_t(pixel >> 8));
            bytes.push_back(uint8_t(pixel));
        }
    }
    panel.WritePixels(bytes.data(), bytes.size());
}

uint16_t Pattern(int x, int y) { return static_cast<uint16_t>((x * 31 + y * 17) ^ (y << 11)); }

}  // namespace

int main()
{
    static St7789 panel;
    std::vector<uint16_t> view(kWidth * kHeight);
    HalSetup(panel);

    std::vector<uint16_t> frame(kWidth * kHeight);
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) frame[y * kWidth + x] = Pattern(x, y);
    // Eight-row bands, as Present sends them.
    for (int y = 0; y < kHeight; y += 8)
    {
        const int rows = y + 8 <= kHeight ? 8 : kHeight - y;
        std::vector<uint16_t> band(frame.begin() + y * kWidth, frame.begin() + (y + rows) * kWidth);
        Draw(panel, 0, y, kWidth, y + rows, band);
    }
    panel.RenderView(view.data());
    assert(view == frame);

    // A region: only its pixels change.
    std::vector<uint16_t> box(10 * 5, 0x07E0);
    Draw(panel, 100, 60, 110, 65, box);
    panel.RenderView(view.data());
    assert(view[60 * kWidth + 100] == 0x07E0 && view[64 * kWidth + 109] == 0x07E0);
    assert(view[59 * kWidth + 100] == Pattern(100, 59) && view[60 * kWidth + 110] == Pattern(110, 60));

    // The window wraps: more pixels than the window go round to its start.
    std::vector<uint16_t> wrap(2 * 2 + 1, 0x001F);
    wrap[4] = 0xF800;
    Draw(panel, 0, 0, 2, 2, wrap);
    panel.RenderView(view.data());
    assert(view[0] == 0xF800 && view[1] == 0x001F);

    // Big-endian data without RAMCTRL's little-endian bit.
    Command(panel, 0xB0, {0x00, 0xF0});
    Draw(panel, 5, 5, 6, 6, {0x1234}, false);
    panel.RenderView(view.data());
    assert(view[5 * kWidth + 5] == 0x1234);

    // Without INVON the IPS glass shows the complement; BGR swaps red and blue; off is black.
    Command(panel, 0x20);
    panel.RenderView(view.data());
    assert(view[5 * kWidth + 5] == static_cast<uint16_t>(~0x1234));
    Command(panel, 0x21);
    Command(panel, 0x36, {0x68});
    panel.RenderView(view.data());
    assert(view[60 * kWidth + 100] == 0x07E0 && view[0] == 0x001F);
    Command(panel, 0x36, {0x60});
    Command(panel, 0x28);
    panel.RenderView(view.data());
    assert(view[60 * kWidth + 100] == 0 && !panel.Showing());

    // Another orientation: no swap, no mirror puts the image somewhere else, as on the device.
    Command(panel, 0x29);
    Command(panel, 0x36, {0x00});
    Draw(panel, 0, 0, 1, 1, {0xFFFF});
    panel.RenderView(view.data());
    assert(view[0] != 0xFFFF || view[1] != 0x001F);
    std::printf("display tests passed\n");
    return 0;
}
