#include "esp_stub.h"
#include "display/display.h"
#include "display/internal/overlay.h"

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
namespace Display = CardenzaHAL::Display;
using Frame = std::array<std::uint16_t, Display::WIDTH * Display::HEIGHT>;

void Require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

Frame MakeFrame()
{
    Frame frame{};
    for (std::size_t index = 0; index < frame.size(); ++index) frame[index] = static_cast<std::uint16_t>(index);
    return frame;
}

void Cold()
{
    const auto frame = MakeFrame();
    Require(Display::SetBrightness(140) && Display::Init(), "cold initialization failed");
    Require(Fake::panelResets == 1 && Fake::panelResetPin == 33, "cold boot did not reset the panel");
    Require(Fake::draws == 0 && Fake::backlightDuty == 0 && !Fake::panelOn,
            "cold initialization revealed pixels before the first frame");
    Require(Display::Init() && Display::SetBrightness(140) && Fake::panelResets == 1 && Fake::backlightDuty == 0,
            "repeated Init or brightness staging revealed the screen");
    Require(Display::Present(frame.data()) && Fake::displayPixels == frame, "full frame was not transferred");
    Require(!Fake::dmaPending && Fake::panelOn && Fake::backlightDuty > 0 && Fake::backlightDuty < 4095,
            "completed frame did not use the requested brightness");
    Require(Fake::backlightRevealedAt == frame.size(), "backlight opened before the whole first frame completed");
    const auto writes = Fake::backlightWrites;
    Require(Display::SetBrightness(140) && Display::Init() && Fake::backlightWrites == writes,
            "unchanged brightness or Init rewrote PWM");
}

void Warm()
{
    Fake::resetReason = ESP_RST_SW;
    Fake::panelOn = true;
    Fake::displayPixels.fill(0x55aa);
    Fake::backlightDuty = 2000;
    const auto before = Fake::displayPixels;
    Require(Display::SetBrightness(100) && Display::Init(), "warm initialization failed");
    Require(Fake::panelResetPin == -1 && Fake::panelResets == 0 && Fake::draws == 0,
            "warm boot reset or cleared the inherited panel");
    Require(Fake::displayPixels == before && Fake::backlightDuty == 2000 && Fake::backlightConfigurations == 0 &&
            Fake::backlightHoldReleases == 0, "warm boot changed inherited pixels or backlight before submission");
    const auto frame = MakeFrame();
    const Display::Region tiny{4, 5, 1, 1};
    Require(Display::Present(frame.data(), &tiny, 1) && Fake::displayPixels == frame,
            "a first partial request did not establish a full frame");
    Require(Fake::backlightRevealedAt == frame.size() && Fake::backlightHoldReleases == 1,
            "warm backlight handoff happened before the first complete frame");
}

void Dirty()
{
    auto frame = MakeFrame();
    Require(Display::Present(frame.data()), "initial full frame failed");
    const auto draws = Fake::draws;
    const Display::Region region{7, 12, 3, 2};
    for (int y = region.y; y < region.y + region.height; ++y)
        for (int x = region.x; x < region.x + region.width; ++x) frame[y * Display::WIDTH + x] = 0xabcd;
    Require(Display::Present(frame.data(), &region, 1), "dirty-region submission failed");
    Require(Fake::draws == draws + 1 && Fake::lastDraw == std::array<int, 4>{7, 12, 10, 14} &&
            Fake::displayPixels == frame, "dirty region used the wrong framebuffer stride or coordinates");
    const auto writes = Fake::backlightWrites;
    Require(Display::Present(frame.data(), nullptr, 0) && Fake::draws == draws + 1 && Fake::backlightWrites == writes,
            "an unchanged frame triggered a transfer or PWM update");
}

void Timeout(bool warm)
{
    if (warm) { Fake::resetReason = ESP_RST_SW; Fake::backlightDuty = 2000; }
    auto frame = MakeFrame();
    Require(Display::SetBrightness(140) && Display::Init(), "timeout fixture failed to initialize");
    Fake::fail = "display_timeout";
    Require(!Display::Present(frame.data()) && Fake::dmaPending, "a missing DMA completion was accepted");
    const auto pending = Fake::pendingPixels;
    const auto* owned = static_cast<const std::uint16_t*>(Fake::lastPixels);
    const auto draws = Fake::draws;
    frame.fill(0xbeef);
    Require(!Display::Present(frame.data()) && Fake::draws == draws && std::equal(pending.begin(), pending.end(), owned),
            "retry overwrote the in-flight DMA band after the caller reused its framebuffer");
    Require(Fake::backlightDuty == (warm ? 2000u : 0u), "timeout changed the startup backlight");
    Fake::fail.clear();
    Fake::CompleteDisplayTransfer();
    Require(Display::Present(frame.data(), nullptr, 0) && Fake::displayPixels == frame && !Fake::dmaPending,
            "timeout recovery did not resend a complete frame");
    Require(Fake::backlightRevealedAt >= frame.size(), "timeout recovery revealed a partial frame");
}

void FailAndRetry(const std::string& scenario)
{
    const auto frame = MakeFrame();
    Require(Display::SetBrightness(140) && Display::Init(), "failure fixture failed to initialize");
    Fake::fail = scenario == "brightness" ? "display_brightness" : scenario;
    Require(!Display::Present(frame.data()), "failed submission or brightness was accepted");
    Require(Fake::backlightDuty == 0, "failed first frame enabled the backlight");
    Fake::fail.clear();
    Require(Display::Present(frame.data()) && Fake::displayPixels == frame && Fake::backlightDuty > 0,
            "first-frame retry could not recover");
    Require(Fake::panelCreates == 1 && Fake::panelResets == 1 && Fake::binarySemaphores == 1,
            "first-frame retry recreated a panel or leaked a completion semaphore");
}

void Native()
{
    const auto frame = MakeFrame();
    Require(Display::SetBrightness(110) && Display::Init(), "native fixture failed to initialize");
    Require(Display::GetPanel() && Display::GetPanelIO() && Fake::callbackRegistrations == 0,
            "native handles were unavailable or HAL took over the caller's callback");
    Require(esp_lcd_panel_draw_bitmap(Display::GetPanel(), 0, 0, Display::WIDTH, Display::HEIGHT, frame.data()) == ESP_OK,
            "native draw failed");
    Fake::fail = "drain";
    Require(!Display::Present() && Fake::backlightDuty == 0 && Fake::dmaPending,
            "failed native drain revealed the frame or released DMA ownership");
    Fake::fail.clear();
    Require(Display::Present() && !Fake::dmaPending && Fake::backlightDuty > 0 && Fake::callbackRegistrations == 0,
            "native submission did not drain before revealing, or replaced the caller's callback");
}

// PresentChanged: the first frame whole, then what changed plus one band of eight rows a frame.
void Changed(bool memory)
{
    if (!memory) Fake::fail = "heap";
    alignas(4) static Frame frame;
    frame = MakeFrame();
    Require(Display::Init() && Display::PresentChanged(frame.data()) && Fake::displayPixels == frame,
            "the first changed-frame present was not the whole screen");
    const std::size_t whole = static_cast<std::size_t>(Display::WIDTH) * Display::HEIGHT;
    std::size_t before = Fake::transferredPixels;
    frame[70 * Display::WIDTH + 100] ^= 0xffff;
    Require(Display::PresentChanged(frame.data()) && Fake::displayPixels == frame, "a changed pixel did not reach the panel");
    const std::size_t sent = Fake::transferredPixels - before;
    if (memory)
        Require(sent <= 30 + Display::WIDTH * 8, "more than the changed segment and one refresh band was sent");
    else
        Require(sent == whole, "without its table the frame was not sent whole");
    // Unchanged frames: only the refresh band, a different one each time, the whole screen in turn.
    before = Fake::transferredPixels;
    Fake::displayPixels.fill(0);
    for (int band = 0; band < 17; ++band) Require(Display::PresentChanged(frame.data()), "an unchanged frame failed");
    if (memory)
    {
        Require(Fake::transferredPixels - before == whole, "the refresh bands did not add up to one screen");
        Require(Fake::displayPixels == frame, "the refresh bands did not bring the whole screen back");
    }
    Fake::fail.clear();
}

// The system menu's overlay (display/internal/overlay.h): its first paint whole, then of each band of eight
// rows only the rectangle over the rows that changed and the span of their 30-pixel segments, packed for
// the panel; without memory for its table, every band whole every time.
void Overlay(bool memory)
{
    alignas(4) static Frame firmware, menu;
    firmware = MakeFrame();
    Require(Display::Init() && Display::Present(firmware.data()), "the firmware's first frame failed");
    if (!memory) Fake::fail = "heap";
    Require(Display::detail::BeginOverlay(), "the overlay did not begin");
    Fake::fail.clear();
    const auto paint = [](void* context, int x, int y, int width, int rows, std::uint16_t* out) {
        const auto& picture = *static_cast<const Frame*>(context);
        for (int row = 0; row < rows; ++row)
            for (int column = 0; column < width; ++column)
                out[row * width + column] = picture[(y + row) * Display::WIDTH + x + column];
    };
    for (std::size_t index = 0; index < menu.size(); ++index) menu[index] = static_cast<std::uint16_t>(~firmware[index]);
    const std::size_t whole = static_cast<std::size_t>(Display::WIDTH) * Display::HEIGHT;
    std::size_t before = Fake::transferredPixels;
    Require(Display::detail::PaintOverlay(paint, &menu) && Fake::displayPixels == menu &&
            Fake::transferredPixels - before == whole, "the overlay's first paint was not the whole screen");
    before = Fake::transferredPixels;
    Require(Display::detail::PaintOverlay(paint, &menu) && Fake::transferredPixels - before == (memory ? 0 : whole),
            memory ? "an unchanged overlay was sent again" : "without its table the overlay was not sent whole");
    // A bar's edge: two rows across 100 columns (four segments, 120 pixels).
    for (int y = 70; y < 72; ++y)
        for (int x = 40; x < 140; ++x) menu[y * Display::WIDTH + x] ^= 0x0f0f;
    before = Fake::transferredPixels;
    Require(Display::detail::PaintOverlay(paint, &menu) && Fake::displayPixels == menu, "an edge did not reach the panel");
    Require(Fake::transferredPixels - before == (memory ? 2 * 120 : whole), "more than the edge's rows and segments went");
    // Two bands, each its own rectangle: rows 6-7 at column 200 (segment 6); rows 8-9 at column 200 and
    // row 9 at column 5 (segments 0 to 6). The rows go packed, each from its own place.
    for (int y = 6; y < 10; ++y) menu[y * Display::WIDTH + 200] ^= 0x00ff;
    menu[9 * Display::WIDTH + 5] ^= 0x00ff;
    before = Fake::transferredPixels;
    Require(Display::detail::PaintOverlay(paint, &menu) && Fake::displayPixels == menu, "two bands' changes were misplaced");
    if (memory)
        Require(Fake::transferredPixels - before == 30 * 2 + 210 * 2 &&
                Fake::lastDraw == std::array<int, 4>{0, 8, 210, 10}, "the bands' rectangles were not the changed ones");
    // A paint that fails leaves the next one sending everything.
    menu[0] ^= 1;
    Fake::fail = "draw";
    Require(!Display::detail::PaintOverlay(paint, &menu), "a failed draw was accepted");
    Fake::fail.clear();
    before = Fake::transferredPixels;
    Require(Display::detail::PaintOverlay(paint, &menu) && Fake::displayPixels == menu &&
            Fake::transferredPixels - before == whole, "after a failed draw the overlay was not sent whole");
    Display::detail::EndOverlay();
    before = Fake::transferredPixels;
    Require(Display::Present(firmware.data(), nullptr, 0) && Fake::displayPixels == firmware &&
            Fake::transferredPixels - before == whole, "the firmware's first frame after the overlay was not whole");
}

void Invalid()
{
    const auto frame = MakeFrame();
    Require(!Display::Present(nullptr) && !Display::Present(frame.data(), nullptr, 1), "invalid input was accepted");
    for (const Display::Region region : {Display::Region{-1, 0, 1, 1}, {0, -1, 1, 1}, {240, 0, 1, 1},
                                        {0, 135, 1, 1}, {0, 0, 0, 1}, {0, 0, 1, -1},
                                        {239, 0, 2, 1}, {0, 0, std::numeric_limits<int>::max(), 1}})
        Require(!Display::Present(frame.data(), &region, 1), "invalid region was accepted");
    Require(Fake::panelCreates == 0 && Fake::draws == 0 && Fake::backlightConfigurations == 0,
            "invalid input changed the hardware");
}
}

int main(int argc, char** argv)
{
    try {
        Require(argc == 2, "missing test scenario");
        const std::string scenario = argv[1];
        if (scenario == "cold") Cold();
        else if (scenario == "warm") Warm();
        else if (scenario == "dirty") Dirty();
        else if (scenario == "timeout" || scenario == "warm_timeout") Timeout(scenario == "warm_timeout");
        else if (scenario == "native") Native();
        else if (scenario == "invalid") Invalid();
        else if (scenario == "changed" || scenario == "changed_no_memory") Changed(scenario == "changed");
        else if (scenario == "overlay" || scenario == "overlay_no_memory") Overlay(scenario == "overlay");
        else FailAndRetry(scenario);
        std::cout << "Display " << scenario << " passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
