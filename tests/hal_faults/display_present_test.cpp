#include "esp_stub.h"
#include "display/display.h"
#include "display/internal/overlay.h"
#include "display/internal/stream.h"

#include <vector>
#include "display/internal/snapshot.h"

#include <array>
#include <cstdint>
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

// The screen stream's hook (display/internal/stream.h): every framebuffer Present* hands it the frame and the
// regions presented (PresentChanged's own, the whole screen while the system menu holds the frames); nothing
// once it is taken away, and no regions for an unchanged Present.
struct HookCall {
    const std::uint16_t* frame;
    std::vector<Display::Region> regions;
};
std::vector<HookCall> hookCalls;

void Hook()
{
    alignas(4) static Frame frame;
    frame = MakeFrame();
    Require(Display::Init() && Display::Present(frame.data()), "the first frame failed");
    Display::detail::SetPresentHook([](const std::uint16_t* framebuffer, const Display::Region* regions, std::size_t count) {
        hookCalls.push_back({framebuffer, std::vector<Display::Region>(regions, regions + count)});
    });
    const Display::Region one{10, 20, 30, 4};
    Require(Display::Present(frame.data(), &one, 1) && hookCalls.size() == 1 && hookCalls[0].frame == frame.data() &&
                hookCalls[0].regions.size() == 1 && hookCalls[0].regions[0].x == 10 && hookCalls[0].regions[0].height == 4,
            "the hook did not see the regions presented");
    Require(Display::Present(frame.data(), nullptr, 0) && hookCalls.size() == 2 && hookCalls[1].regions.empty(),
            "an unchanged Present did not reach the hook without regions");
    // PresentChanged: what it found (one changed pixel's segment) and its refresh band.
    Require(Display::PresentChanged(frame.data()), "the first changed frame failed");
    frame[70 * Display::WIDTH + 100] ^= 0xffff;
    const std::size_t before = hookCalls.size();
    Require(Display::PresentChanged(frame.data()) && hookCalls.size() == before + 1, "PresentChanged did not call the hook");
    bool covered = false;
    for (const auto& region : hookCalls.back().regions)
        covered = covered || (region.x <= 100 && 100 < region.x + region.width && region.y <= 70 && 70 < region.y + region.height);
    Require(covered, "PresentChanged's regions did not cover the change");
    // While the system menu holds the frames: the whole screen.
    Require(Display::detail::BeginOverlay(), "the overlay did not begin");
    Require(Display::PresentChanged(frame.data()) && hookCalls.back().regions.size() == 1 &&
                hookCalls.back().regions[0].width == Display::WIDTH && hookCalls.back().regions[0].height == Display::HEIGHT,
            "a held frame did not reach the hook whole");
    Display::detail::EndOverlay();
    Display::detail::SetPresentHook(nullptr);
    const std::size_t after = hookCalls.size();
    Require(Display::Present(frame.data()) && Display::PresentChanged(frame.data()) && hookCalls.size() == after,
            "the hook was called after it was taken away");
}

// The system menu's backlight (display/internal/overlay.h SetBacklightDuty): its own duty while it shows,
// whatever brightness the firmware sets meanwhile, and the firmware's again when the overlay ends.
void OverlayBacklight()
{
    alignas(4) static Frame firmware;
    firmware = MakeFrame();
    Require(Display::SetBrightness(140) && Display::Init() && Display::Present(firmware.data()), "the firmware's first frame failed");
    const auto paint = [](void* context, int x, int y, int width, int rows, std::uint16_t* out) {
        const auto& picture = *static_cast<const Frame*>(context);
        for (int row = 0; row < rows; ++row)
            for (int column = 0; column < width; ++column)
                out[row * width + column] = picture[(y + row) * Display::WIDTH + x + column];
    };
    Require(Display::detail::BeginOverlay() && Display::detail::PaintOverlay(paint, &firmware), "the overlay did not paint");
    const unsigned firmwareDuty = Fake::backlightDuty;
    Display::detail::SetBacklightDuty(4095, true);
    Require(Fake::backlightDuty == 4095 && Display::detail::FirmwareBacklightDuty() == static_cast<int>(firmwareDuty),
            "the menu's duty was not applied");
    Require(Display::SetBrightness(20) && Fake::backlightDuty == 4095, "the firmware changed the backlight under the menu");
    Display::detail::SetBacklightDuty(0, true);
    Require(Fake::backlightDuty == 0, "the menu could not turn the backlight off");
    Display::detail::EndOverlay();
    Require(Fake::backlightDuty < firmwareDuty && Fake::backlightDuty > 0, "the firmware's brightness did not come back");
}

// Every framebuffer Present* goes out in the panel's scan order (display/scan_order.h): a whole frame as 15
// windows of 16 columns from left to right, through two DMA blocks taken in turn (one when the heap has no
// room for the second, or when the options say so).
void Scan(bool secondBlock)
{
    if (!secondBlock) Fake::noDmaHeap = true;
    const auto frame = MakeFrame();
    Fake::drawLog.clear();
    Fake::blockLog.clear();
    Require(Display::Present(frame.data()) && Fake::displayPixels == frame, "a whole frame did not reach the panel");
    Require(Fake::drawLog.size() == 15, "a whole frame was not 15 windows");
    for (std::size_t index = 0; index < Fake::drawLog.size(); ++index)
        Require(Fake::drawLog[index] == std::array<int, 4>{static_cast<int>(index) * 16, 0, static_cast<int>(index) * 16 + 16,
                                                            Display::HEIGHT},
                "the windows were not the screen's columns from left to right");
    for (std::size_t index = 1; index < Fake::blockLog.size(); ++index)
        Require((Fake::blockLog[index] != Fake::blockLog[index - 1]) == secondBlock,
                secondBlock ? "consecutive windows did not take the two blocks in turn" : "one block was not reused");
    Require(Fake::dmaBlocks == (secondBlock ? 1 : 0), "the second block was not taken once");
    // A short strip across the screen is one window; a tall narrow rectangle a column band.
    Fake::drawLog.clear();
    const Display::Region strip{0, 40, Display::WIDTH, 8}, column{100, 0, 10, Display::HEIGHT};
    const Display::Region both[] = {column, strip};
    Require(Display::Present(frame.data(), both, 2), "two rectangles failed");
    Require(Fake::drawLog.size() == 2 && Fake::drawLog[0] == std::array<int, 4>{0, 40, 240, 48} &&
                Fake::drawLog[1] == std::array<int, 4>{100, 0, 110, Display::HEIGHT},
            "the rectangles did not go out left to right, each in its window");
    if (secondBlock)
    {
        // The options can give the second block back: one block from then on.
        Display::PresentOptions options = Display::GetPresentOptions();
        options.pipeline = false;
        Require(Display::SetPresentOptions(options), "valid options were refused");
        Fake::blockLog.clear();
        Require(Display::Present(frame.data()) && Fake::blockLog.size() == 15, "a whole frame failed with one block");
        for (const void* block : Fake::blockLog) Require(block == Fake::blockLog.front(), "one block was not reused");
    }
    Fake::noDmaHeap = false;
}

// FrameStats, PresentTotals and the frame callback (display/presenter.h).
void Stats()
{
    alignas(4) static Frame frame;
    frame = MakeFrame();
    struct Seen { int calls = 0; Display::FrameStats last; } seen;
    Display::SetFrameCallback([](const Display::FrameStats& stats, void* context) {
        auto& record = *static_cast<Seen*>(context);
        ++record.calls;
        record.last = stats;
        // The callback runs outside the display's lock: it may ask the display.
        Require(Display::LastFrame().number == stats.number, "the callback could not read the last frame");
    }, &seen);
    Require(Display::Init() && Display::PresentChanged(frame.data()), "the first frame failed");
    auto stats = Display::LastFrame();
    Require(stats.ok && stats.whole && !stats.held && stats.number == 1 && stats.bytes == 64800 && stats.windows == 15,
            "the first frame's stats were wrong");
    Require(seen.calls == 1 && seen.last.number == 1 && seen.last.bytes == 64800, "the callback did not see the first frame");
    frame[70 * Display::WIDTH + 100] ^= 0xffff;
    Require(Display::PresentChanged(frame.data()), "a changed frame failed");
    stats = Display::LastFrame();
    Require(stats.ok && !stats.whole && stats.number == 2 && stats.regions == 2 && stats.windows == 2 &&
                stats.bytes == (30 + Display::WIDTH * 8) * 2,
            "a changed pixel's stats were not its segment and the refresh band");
    // The system menu holds the screen: held, nothing sent.
    Require(Display::detail::BeginOverlay(), "the overlay did not begin");
    Require(Display::PresentChanged(frame.data()) && Display::LastFrame().held && Display::LastFrame().bytes == 0,
            "a held frame was not reported as held");
    Display::detail::EndOverlay();
    // A failed frame, then the whole screen after it.
    Fake::fail = "draw";
    Require(!Display::PresentChanged(frame.data()) && !Display::LastFrame().ok, "a failed frame was reported as sent");
    Fake::fail.clear();
    Require(Display::PresentChanged(frame.data()) && Display::LastFrame().whole, "the frame after a failure was not whole");
    auto totals = Display::Totals();
    Require(totals.frames == 5 && totals.wholeFrames == 3 && totals.heldFrames == 1 && totals.failures == 1 &&
                totals.bytes == 64800 * 2 + (30 + Display::WIDTH * 8) * 2 && seen.calls == 5,
            "the totals did not add up");
    Display::ResetTotals();
    Require(Display::Totals().frames == 0 && Display::Totals().bytes == 0, "the totals were not reset");
    Display::SetFrameCallback(nullptr);
    Require(Display::PresentChanged(frame.data()) && seen.calls == 5, "a removed callback still ran");
}

// PresentOptions and RequestWholeFrame.
void Options()
{
    alignas(4) static Frame frame;
    frame = MakeFrame();
    Require(Display::Init() && Display::PresentChanged(frame.data()), "the first frame failed");
    Display::PresentOptions options;
    options.segmentPixels = 7;
    Require(!Display::SetPresentOptions(options), "a segment of 7 pixels was accepted");
    options.segmentPixels = 16;
    options.refreshBands = 18;
    Require(!Display::SetPresentOptions(options), "18 refresh bands were accepted");
    options.refreshBands = -1;
    Require(!Display::SetPresentOptions(options), "negative refresh bands were accepted");
    Require(Display::GetPresentOptions().segmentPixels == 30, "a refused option changed the options");
    options.refreshBands = 0;
    Require(Display::SetPresentOptions(options) && Display::GetPresentOptions().segmentPixels == 16,
            "valid options were refused");
    Require(Display::PresentChanged(frame.data()) && Display::LastFrame().whole, "new options did not start with a whole frame");
    Require(Display::PresentChanged(frame.data()) && Display::LastFrame().bytes == 0 && Display::LastFrame().windows == 0,
            "without refresh bands an unchanged frame sent something");
    frame[20 * Display::WIDTH + 33] ^= 1;
    Fake::drawLog.clear();
    Require(Display::PresentChanged(frame.data()) && Fake::drawLog.size() == 1 &&
                Fake::drawLog[0] == std::array<int, 4>{32, 20, 48, 21} && Fake::displayPixels == frame,
            "a changed pixel did not go out as its 16-pixel segment");
    Display::RequestWholeFrame();
    Require(Display::PresentChanged(frame.data()) && Display::LastFrame().whole && Display::LastFrame().bytes == 64800,
            "RequestWholeFrame did not send the whole screen");
    Require(Display::PresentChanged(frame.data()) && Display::LastFrame().bytes == 0, "the frame after it was not back to changes");
}

// FramePacer on the fake clock: draws every other tick, sleeps to each tick, gives way after three late ones.
void Pacer()
{
    Fake::now = 5000000;  // tick 5000
    Display::PacerOptions options;
    Display::FramePacer pacer(options);
    Require(pacer.Period() == 10 && pacer.DrawTick() && pacer.Tick() == 0, "the pacer did not start on a drawing tick");
    Require(pacer.Wait() && Fake::now == 5010000 && !pacer.DrawTick(), "an on-time tick did not sleep to the next");
    Require(pacer.Wait() && Fake::now == 5020000 && pacer.DrawTick(), "the second tick did not sleep to the next");
    // Each tick now takes 15 ms: late, no sleep; the third late one in a row sleeps one RTOS tick.
    Fake::lateBy = 15000;
    Require(!pacer.Wait() && !pacer.Wait(), "late ticks slept");
    const auto before = Fake::now;
    Require(!pacer.Wait() && Fake::now == before + 15000 + 1000 && pacer.Yields() == 1 && pacer.LateTicks() == 3,
            "the third late tick did not give way for one RTOS tick");
    // Back on time: the schedule starts again from the tick after giving way.
    Fake::lateBy = 0;
    const auto restart = Fake::now;
    Require(pacer.Wait() && Fake::now == restart + 10000, "the schedule did not restart from giving way");
}

// AllocateFramebuffer and DoubleBuffer (on the fakes the presenting task does not run: what can be checked is
// what it keeps and gives back).
void Buffers()
{
    uint16_t* frame = Display::AllocateFramebuffer();
    Require(frame != nullptr && reinterpret_cast<std::uintptr_t>(frame) % 32 == 0, "a framebuffer was not 32-byte aligned");
    for (int index = 0; index < Display::WIDTH * Display::HEIGHT; ++index) Require(frame[index] == 0, "a framebuffer was not zeroed");
    Display::FreeFramebuffer(frame);
    Fake::fail = "heap";
    Require(Display::AllocateFramebuffer(Display::FrameMemory::Internal) == nullptr, "no room gave a framebuffer");
    {
        Display::DoubleBuffer chain;
        Require(!chain.Begin() && chain.Back() == nullptr && Fake::tasks == 0 && Fake::binarySemaphores == 0,
                "a double buffer without room kept something");
    }
    Fake::fail.clear();
    {
        Display::DoubleBuffer chain;
        Require(chain.Begin() && Fake::tasks == 1 && Fake::binarySemaphores == 2, "a double buffer did not begin");
        uint16_t* first = chain.Back();
        Require(first != nullptr && chain.Present() && chain.Back() != nullptr && chain.Back() != first,
                "Present did not hand over the back frame");
        chain.End();
        Require(Fake::tasks == 0 && Fake::binarySemaphores == 0 && chain.Back() == nullptr, "End did not give everything back");
    }
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

// A firmware that draws through the panel's handles itself: what it draws is kept for a screenshot
// (Display::detail::PanelDrawsCopy), the HAL's own sends are not.
void PanelDraws()
{
    Require(Display::Init(), "initialization failed");
    Require(Display::detail::PanelDrawsCopy() == nullptr, "a copy before any draw");
    const auto frame = MakeFrame();
    Require(Display::Present(frame.data()), "a framebuffer's frame failed");
    Require(Display::detail::PanelDrawsCopy() == nullptr, "the HAL's own send was copied as a panel draw");
    std::array<std::uint16_t, 10 * 4> block{};
    for (std::size_t i = 0; i < block.size(); ++i) block[i] = static_cast<std::uint16_t>(0xA000 + i);
    Require(esp_lcd_panel_draw_bitmap(Display::GetPanel(), 20, 30, 30, 34, block.data()) == ESP_OK, "a panel draw failed");
    const std::uint16_t* copy = Display::detail::PanelDrawsCopy();
    Require(copy != nullptr, "the panel draw was not kept");
    for (int y = 30; y < 34; ++y)
        for (int x = 20; x < 30; ++x)
            Require(copy[y * Display::WIDTH + x] == block[(y - 30) * 10 + (x - 20)], "the kept pixels differ");
    Require(copy[29 * Display::WIDTH + 20] == 0 && copy[30 * Display::WIDTH + 30] == 0, "pixels outside the draw changed");
    Require(Fake::displayPixels[30 * 240 + 20] == block[0], "the draw did not reach the panel");
    // At the screen's edge.
    Require(esp_lcd_panel_draw_bitmap(Display::GetPanel(), 230, 131, 240, 135, block.data()) == ESP_OK, "an edge draw failed");
    Require(copy[134 * Display::WIDTH + 239] == block[39], "the edge draw was not kept");
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
        else if (scenario == "overlay_backlight") OverlayBacklight();
        else if (scenario == "scan" || scenario == "scan_one_block") Scan(scenario == "scan");
        else if (scenario == "stats") Stats();
        else if (scenario == "options") Options();
        else if (scenario == "pacer") Pacer();
        else if (scenario == "buffers") Buffers();
        else if (scenario == "hook") Hook();
        else if (scenario == "panel_draws") PanelDraws();
        else FailAndRetry(scenario);
        std::cout << "Display " << scenario << " passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
