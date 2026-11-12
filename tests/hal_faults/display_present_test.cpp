#include "esp_stub.h"
#include "display/display.h"

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
        else FailAndRetry(scenario);
        std::cout << "Display " << scenario << " passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
