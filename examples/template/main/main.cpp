// A starting point for a Cardenza firmware on CardenzaHAL: replace it with yours.
//
// - The screen shows the firmware's name and version and the 4 x 14 keys; a held key fills its cell and
//   lights its LED, the others glow faintly.
// - HOME (the side key) inverts the screen.
// - Keys: keyboard/legends.h says what each cap prints (Legend, Typed; the arrows are ; . , / and ok
//   confirms, esc goes back). Use it rather than a layout of your own.
//
// It reaches the hardware only through the HAL's public headers, so the same source builds for the board
// (idf.py build) and for the browser (CardenzaHAL tools/cardenza_web.py build). Keep CardenzaHAL::Init()
// first: its log lines are how a crash log, and the 203 community's checks, know the HAL.

#include <cstdint>
#include <cstring>
#include <vector>

#include <esp_app_desc.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "cardenzaHAL.h"
#include "keyboard/led_light.h"

namespace {

namespace Display = CardenzaHAL::Display;
namespace Keyboard = CardenzaHAL::Keyboard;
namespace System = CardenzaHAL::System;

constexpr char kTag[] = "app";
constexpr int kColumns = 14;
constexpr int kRows = 4;
constexpr int kCellWidth = 16;
constexpr int kCellHeight = 22;
constexpr int kGridX = (Display::WIDTH - kColumns * kCellWidth) / 2;
constexpr int kGridY = 36;

constexpr uint16_t Rgb(int r, int g, int b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// The framebuffer: 64,800 bytes, static (not on the task's stack), 4-byte aligned for PresentChanged.
alignas(4) uint16_t g_frame[Display::WIDTH * Display::HEIGHT];

void Fill(int x, int y, int width, int height, uint16_t colour)
{
    for (int row = y; row < y + height && row < Display::HEIGHT; ++row)
        for (int column = x; column < x + width && column < Display::WIDTH; ++column) g_frame[row * Display::WIDTH + column] = colour;
}

void Outline(int x, int y, int width, int height, uint16_t colour)
{
    Fill(x, y, width, 1, colour);
    Fill(x, y + height - 1, width, 1, colour);
    Fill(x, y, 1, height, colour);
    Fill(x + width - 1, y, 1, height, colour);
}

void Draw(const std::vector<Keyboard::Point2D_t>& keys, bool inverted, int versionLength)
{
    const uint16_t background = inverted ? Rgb(232, 232, 224) : Rgb(0, 0, 0);
    const uint16_t ink = inverted ? Rgb(0, 0, 0) : Rgb(232, 232, 224);
    const uint16_t faint = Rgb(58, 58, 54);
    Fill(0, 0, Display::WIDTH, Display::HEIGHT, background);
    // No font here (bring your own): a bar as long as the version string stands for the title.
    Fill(kGridX, 12, 6 * versionLength, 8, Rgb(255, 208, 48));
    for (int y = 0; y < kRows; ++y)
        for (int x = 0; x < kColumns; ++x)
        {
            const int left = kGridX + x * kCellWidth;
            const int top = kGridY + y * kCellHeight;
            bool held = false;
            for (const auto& key : keys) held |= key.x == x && key.y == y;
            if (held) Fill(left + 1, top + 1, kCellWidth - 2, kCellHeight - 2, ink);
            else Outline(left + 1, top + 1, kCellWidth - 2, kCellHeight - 2, faint);
        }
}

}  // namespace

extern "C" void app_main()
{
    if (!CardenzaHAL::Init())
    {
        const auto& report = CardenzaHAL::LastInitReport();
        ESP_LOGE(kTag, "HAL init failed: system=%d keyboard=%d audio=%d display=%d", report.system, report.keyboard, report.audio,
                 report.display);
        return;
    }
    // The first line of every start says what is running: the app description's name and version
    // (version.txt). Keep the ELF of every build you give anyone: a backtrace is read with it.
    const esp_app_desc_t* app = esp_app_get_description();
    ESP_LOGI(kTag, "%s %s ready", app->project_name, app->version);

    Display::SetBrightness(200);
    Keyboard::enableLED(true);
    Keyboard::setLEDGlobalBrightness(15);

    bool inverted = false;
    bool homeBefore = false;
    const int versionLength = static_cast<int>(std::strlen(app->version));
    TickType_t wake = xTaskGetTickCount();
    for (;;)
    {
        Keyboard::updateKeyList();
        const auto& keys = Keyboard::keyList();
        const bool home = System::GetHomeButton();
        if (home && !homeBefore) inverted = !inverted;
        homeBefore = home;

        Keyboard::LedFrame leds{};
        leds.fill(Keyboard::Light::PerceivedToLevel(4));
        for (const auto& key : keys) leds[key.y * Keyboard::LED_COLUMNS + key.x] = Keyboard::Light::PerceivedToLevel(15);
        Keyboard::setLEDFrame(leds);

        Draw(keys, inverted, versionLength);
        Display::PresentChanged(g_frame);  // only what changed: a whole frame is 17 ms of SPI
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(20));  // 50 fps
    }
}
