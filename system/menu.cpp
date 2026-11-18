// The system menu (system/menu.h): long-press HOME in any firmware on the HAL.
#include "system/menu.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <soc/io_mux_reg.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/rtc_cntl_struct.h>

#include "board/pins.h"
#include "display/backlight_brightness.h"
#include "display/display.h"
#include "display/internal/overlay.h"
#include "keyboard/internal/overlay.h"
#include "system/internal/hooks.h"
#include "system/launcher.h"
#include "system/menu/frame.h"
#include "system/menu/launcher/motion.h"
#include "system/menu/logic.h"
#include "system/menu/test.h"

namespace CardenzaHAL::SystemMenu {

namespace {

constexpr char kTag[] = "CardenzaHAL";
constexpr uint32_t kPollMs = 20;      // the side key, while the menu is closed
constexpr uint32_t kMenuPollMs = 10;  // keys while it is open
constexpr uint32_t kCurtainFrameMs = 5;  // the curtain's frames: a few columns each, one short write
// The Launcher's text code works with String and glyph buffers on the stack.
constexpr uint32_t kStackBytes = 5120;
// Above a firmware's UI loop (usually 1 to 3), below audio and USB: the menu answers while the firmware
// is busy drawing, and the firmware's sound keeps going.
constexpr UBaseType_t kPriority = 4;

std::atomic<bool> g_started{false};
TaskHandle_t g_task = nullptr;
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_open{false};
std::atomic<uint8_t> g_injectedKey{0};  // system/menu/test.h
portMUX_TYPE g_hooksMux = portMUX_INITIALIZER_UNLOCKED;
Listener g_listener = nullptr;
void* g_listenerContext = nullptr;
LeaveHook g_leaveHook = nullptr;
void* g_leaveContext = nullptr;

uint32_t NowMs()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

}  // namespace

// The Launcher's clock, for its motion (launcher/motion.h).
namespace launcher {
uint32_t launcherMillis()
{
    return NowMs();
}
}  // namespace launcher

namespace {

// ---- The Launcher's settings: NVS launcher/lang and launcher/bright, read page by page without the NVS
// library (nothing is written) -----------------------------------------------------------------------------

struct LauncherSettings {
    int language = 0;      // its launcher/lang
    int brightness = 100;  // its backlight, percent
};

LauncherSettings ReadLauncherSettings()
{
    LauncherSettings settings;
    const esp_partition_t* nvs =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
    if (nvs == nullptr) return settings;
    auto* page = static_cast<uint8_t*>(heap_caps_malloc(detail::kNvsPageBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (page == nullptr) page = static_cast<uint8_t*>(heap_caps_malloc(detail::kNvsPageBytes, MALLOC_CAP_8BIT));
    if (page == nullptr) return settings;
    const std::size_t pages = nvs->size / detail::kNvsPageBytes;
    uint8_t index = 0;
    for (std::size_t i = 0; i < pages && index == 0; ++i)
        if (esp_partition_read(nvs, i * detail::kNvsPageBytes, page, detail::kNvsPageBytes) == ESP_OK)
            index = detail::NvsNamespaceIndex(page, "launcher");
    uint8_t lang = 0;
    int32_t bright = 0;
    bool langFound = false, brightFound = false;
    for (std::size_t i = 0; i < pages && index != 0 && !(langFound && brightFound); ++i)
        if (esp_partition_read(nvs, i * detail::kNvsPageBytes, page, detail::kNvsPageBytes) == ESP_OK)
        {
            langFound = langFound || detail::NvsFindU8(page, index, "lang", lang);
            brightFound = brightFound || detail::NvsFindI32(page, index, "bright", bright);
        }
    heap_caps_free(page);
    settings.language = langFound ? lang : 0;
    settings.brightness = detail::LauncherBrightness(brightFound, bright);
    return settings;
}

// ---- The picture ---------------------------------------------------------------------------------------

// What the screen shows: the menu (drawn by the Launcher's code, frame.cpp), in its place, and from `edge` on
// the firmware's picture (its last frame, captured) mixed with it.
struct Scene {
    detail::View view;
    launcher::ListSlide slide;          // the selection bar's motion, once the menu is open
    bool useSlide = false;
    uint32_t now = 0;                   // the menu's frame is drawn at this time
    int edge = detail::kWidth;          // from here on: firmware and menu mixed
    int mix = 256;                      // 0 the firmware's picture, 256 the menu
    const uint16_t* firmware = nullptr; // null: no picture (right of edge is the menu)
    // The menu's picture, drawn once a frame (PSRAM): the curtain and the pop show the one Prepare drew (the
    // first row selected) frame after frame, and drawing the Launcher's frame again for every
    // band of every step kept the CPU so busy that the firmware's own tasks below this one stalled (the
    // board's self-test, 2026-11-18); in the menu each frame of the selection bar's slide is drawn into it
    // (DrawMenu). Null without PSRAM: every band draws the frame (the same `now`, the same picture).
    uint8_t* still = nullptr;
    uint8_t grey[detail::kWidth * 8];   // one band of the menu
};

Scene* g_scene = nullptr;
uint16_t* g_capture = nullptr;  // the firmware's frame, PSRAM (no PSRAM: no fades over its picture)
int g_curtain = 0;              // the curtain's edge on the screen
// The backlight's duty while the menu shows: the Launcher's own (its brightness setting on its curve).
int g_launcherBrightness = 100;  // percent
int g_menuDuty = Display::detail::kBacklightFullDuty;
char g_firmwareName[80];

void PaintScene(void* context, int x, int y, int width, int rows, uint16_t* out)
{
    Scene& scene = *static_cast<Scene*>(context);
    const uint8_t* menu = scene.grey;
    if (scene.still != nullptr)
        menu = scene.still + y * detail::kWidth;
    else
        detail::PaintGrey(scene.view, scene.useSlide ? &scene.slide : nullptr, scene.now, y, rows, scene.grey);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < width; ++c)
        {
            const int px = x + c;
            uint16_t pixel = detail::PanelGrey(menu[r * detail::kWidth + px]);
            if (px >= scene.edge && scene.firmware != nullptr)
                pixel = detail::PanelMix(scene.firmware[(y + r) * detail::kWidth + px], pixel, scene.mix);
            out[r * width + c] = pixel;
        }
}

// The scene for a pull or the menu: the Launcher's language, the firmware's name, the first row selected;
// and a buffer the firmware's frames are copied into (a fade needs its picture).
bool Prepare()
{
    if (g_scene == nullptr)
    {
        g_scene = new (std::nothrow) Scene();
        if (g_scene == nullptr)
        {
            ESP_LOGE(kTag, "system menu: no memory");
            return false;
        }
    }
    const esp_app_desc_t* app = esp_app_get_description();
    std::snprintf(g_firmwareName, sizeof(g_firmwareName), "%.32s %.32s", app->project_name, app->version);
    g_scene->view = detail::View{};
    const LauncherSettings launcherSettings = ReadLauncherSettings();
    g_launcherBrightness = launcherSettings.brightness;
    g_menuDuty = detail::LauncherBacklightDuty(launcherSettings.brightness, Display::detail::kBacklightFullDuty);
    g_scene->view.language = detail::LanguageIndex(launcherSettings.language);
    g_scene->view.firmware = g_firmwareName;
    g_scene->useSlide = false;
    if (g_scene->still == nullptr)
        g_scene->still = static_cast<uint8_t*>(heap_caps_malloc(std::size_t(detail::kWidth) * detail::kHeight,
                                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_scene->still != nullptr)
        detail::PaintGrey(g_scene->view, nullptr, NowMs(), 0, detail::kHeight, g_scene->still);
    if (g_capture == nullptr)
    {
        g_capture = static_cast<uint16_t*>(heap_caps_malloc(
            std::size_t(detail::kWidth) * detail::kHeight * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (g_capture != nullptr) Display::detail::SetCapture(g_capture);
    }
    return true;
}

void Release()
{
    Display::detail::SetCapture(nullptr);
    heap_caps_free(g_capture);
    g_capture = nullptr;
    if (g_scene != nullptr) heap_caps_free(g_scene->still);
    delete g_scene;
    g_scene = nullptr;
}

const uint16_t* FirmwarePicture()
{
    return g_capture != nullptr && Display::detail::CaptureValid() ? g_capture : nullptr;
}

// The menu's frame at `now`, then sent (only what changed of each band goes out, Display::detail::
// PaintOverlay). With PSRAM the Launcher's frame is drawn once into `still`; without, every band draws it.
bool DrawMenu(Scene& scene, uint32_t now)
{
    scene.now = now;
    if (scene.still != nullptr)
        detail::PaintGrey(scene.view, scene.useSlide ? &scene.slide : nullptr, now, 0, detail::kHeight, scene.still);
    return Display::detail::PaintOverlay(PaintScene, &scene);
}

// ---- The curtain ---------------------------------------------------------------------------------------

// The curtain to `edge` pixels: the menu is in its place and uncovered up to the edge; the firmware keeps the
// rest of the screen. Only the columns the edge passed are sent (the panel has no tearing-effect line; a strip
// of few columns goes in one write along its scan and shows no seam): going out, the menu's; going back, the
// firmware's picture (its last frame), after which its own frames reach them again.
void DrawCurtain(int edge)
{
    edge = std::clamp(edge, 0, detail::kWidth);
    if (edge == g_curtain) return;
    if (g_curtain == 0)
    {
        if (!Display::detail::Ready() || !Prepare()) return;
    }
    Scene& scene = *g_scene;
    scene.now = NowMs();
    scene.edge = edge;
    scene.mix = 0;
    scene.firmware = FirmwarePicture();
    // With the still picture a strip is painted in one piece; without, the Launcher's frame is drawn for
    // each band of eight rows.
    (void)Display::detail::MoveCurtain(PaintScene, &scene, edge, scene.firmware != nullptr,
                                       scene.still != nullptr ? detail::kHeight : 8);
    g_curtain = edge;
    if (edge == 0) Release();
}

// ---- Leaving -------------------------------------------------------------------------------------------

[[noreturn]] void PowerOff()
{
    ESP_LOGW(kTag, "system menu: turning off (HOME turns it on)");
    (void)Display::detail::SleepPanel();
    Keyboard::detail::LightsOffForSleep();
    gpio_deep_sleep_hold_en();
    const auto key = static_cast<gpio_num_t>(Pins::kHomeButton);
    (void)rtc_gpio_pullup_en(key);
    (void)rtc_gpio_pulldown_dis(key);
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    (void)esp_sleep_enable_ext0_wakeup(key, 0);
    // No Launcher ticket: esp_deep_sleep_start syncs the RTC timer's time over it (system/launcher.cpp).
    // HOME's wake then starts this firmware again, as the Launcher's bootloader does after a firmware's sleep.
    esp_deep_sleep_start();
}

[[noreturn]] void Leave(Action action)
{
    LeaveHook hook;
    void* context;
    portENTER_CRITICAL(&g_hooksMux);
    hook = g_leaveHook;
    context = g_leaveContext;
    portEXIT_CRITICAL(&g_hooksMux);
    if (hook) hook(action, context);
    // A USB host sees the device leave, as with the USB commands (usb/usb.cpp).
    System::detail::StopUsb();
    switch (action)
    {
        case Action::Launcher:
            System::RestartIntoLauncher();
        case Action::Restart:
            ESP_LOGW(kTag, "system menu: restarting");
            System::detail::RestartWithTicket(System::LauncherTicket::kBoot);
        case Action::Download:
            ESP_LOGW(kTag, "system menu: restarting into the ROM download mode");
            // USB-Serial-JTAG back on the PHY (this survives the restart) and the ROM stays in download mode.
            RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 0;
            REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
            esp_restart();
        case Action::PowerOff:
            PowerOff();
    }
    esp_restart();
}

// ---- The menu ------------------------------------------------------------------------------------------

void Notify(bool open)
{
    Listener listener;
    void* context;
    portENTER_CRITICAL(&g_hooksMux);
    listener = g_listener;
    context = g_listenerContext;
    portEXIT_CRITICAL(&g_hooksMux);
    if (listener) listener(open, context);
}

detail::Key HeldKey()
{
    Keyboard::Point2D_t keys[8];
    const std::size_t count = Keyboard::detail::OverlayKeys(keys, 8);
    for (std::size_t i = 0; i < count; ++i)
    {
        const detail::Key key = detail::KeyAt(keys[i].x, keys[i].y);
        if (key != detail::Key::None) return key;
    }
    return detail::Key::None;
}

int Between(int from, int to, float part)
{
    return from + int(std::lround((to - from) * std::clamp(part, 0.f, 1.f)));
}

// Frames of a fade, `ms` long, eased as the Launcher eases (ease: cubic out): frame(part) for each, the
// last with part 1.
template <typename Frame>
void Fade(uint32_t ms, Frame frame)
{
    const uint32_t start = NowMs();
    while (true)
    {
        const uint32_t elapsed = NowMs() - start;
        const float part = elapsed >= ms ? 1.f : launcher::ease(float(elapsed) / float(ms));
        frame(part);
        if (elapsed >= ms) return;
        vTaskDelay(1);
    }
}

void RunMenu()
{
    if (g_scene == nullptr && !Prepare()) return;
    Scene& scene = *g_scene;
    ESP_LOGI(kTag, "system menu: open");
    System::detail::HoldHomeButton(true);
    g_open.store(true);
    Notify(true);
    Keyboard::LedFrame lights{};
    for (const auto& key : detail::kNavigationKeys)
        lights[static_cast<std::size_t>(key.y) * Keyboard::LED_COLUMNS + key.x] = Keyboard::LED_BRIGHTNESS_MAX;
    Keyboard::detail::BeginOverlay(lights);
    // `curtain`: the menu's left part stays on the screen up to there (MenuTask takes it back to the left).
    const auto close = [&](int curtain) {
        Keyboard::detail::EndOverlay();
        g_open.store(false);
        g_curtain = curtain;
        if (curtain == 0) Release();
        System::detail::HoldHomeButton(false);
        Notify(false);
    };
    if (!Display::detail::BeginOverlay())
    {
        // No screen to show it on (the firmware never set the display up): nothing to choose from.
        ESP_LOGE(kTag, "system menu: no display");
        (void)Display::detail::MoveCurtain(nullptr, nullptr, 0, false);
        close(0);
        return;
    }

    // The pop: the rest of the menu fades in over the firmware's picture (the curtain is already the menu's
    // left part, where it stays), and the backlight goes to the Launcher's, on the Launcher's PWM; without
    // the firmware's picture the curtain opens the rest quickly instead.
    const int firmwareDuty = Display::detail::FirmwareBacklightDuty();
    const int menuDuty = g_menuDuty;
    scene.firmware = FirmwarePicture();
    if (scene.firmware != nullptr)
    {
        scene.edge = g_curtain;
        Fade(detail::kPopMs, [&](float part) {
            scene.now = NowMs();
            scene.mix = Between(0, 256, part);
            Display::detail::SetBacklightDuty(Between(firmwareDuty, menuDuty, part), true);
            (void)Display::detail::PaintOverlay(PaintScene, &scene);
        });
    }
    else
    {
        scene.edge = detail::kWidth;
        int shown = g_curtain;
        Fade(detail::kPopMs, [&](float part) {
            scene.now = NowMs();
            Display::detail::SetBacklightDuty(Between(firmwareDuty, menuDuty, part), true);
            const int edge = Between(g_curtain, detail::kWidth, part);
            (void)Display::detail::PaintColumns(PaintScene, &scene, shown, edge - shown,
                                                scene.still != nullptr ? detail::kHeight : 8);
            shown = edge;
        });
    }
    scene.edge = detail::kWidth;
    scene.mix = 256;
    // The Launcher's lists on the Cardenza: the selection bar slides by its edges (ListSlide), aimed at the
    // selected row as each frame is drawn (the first frame places it); past the last row the rows change in
    // one frame and the bar keeps its place on screen, sliding on from there (keep).
    scene.slide = launcher::ListSlide{};
    scene.slide.keep = true;
    scene.useSlide = true;
    if (!DrawMenu(scene, NowMs())) ESP_LOGW(kTag, "system menu: a paint failed");

    detail::KeyRepeat repeat;
    bool openingPress = true;  // the press that opened the menu: its release is no tap
    bool funcWasDown = true;
    bool moving = false;       // the frame last drawn is not the bar's last: draw the next one soon
    detail::Item chosen = detail::Item::Continue;
    while (true)
    {
        // While the bar moves, frames follow one another (the Launcher's 1 ms; a tick here): the panel has
        // no tearing-effect line, so a frame may be written across its scan, and the smaller each step, the
        // less a seam can show. Each band sends only what changed: the strips the bar's edges swept. Without
        // PSRAM every band draws the frame: one every kMenuPollMs then. Otherwise the keys every kMenuPollMs.
        vTaskDelay(moving && scene.still != nullptr ? 1 : pdMS_TO_TICKS(kMenuPollMs));
        if (!g_enabled.load()) break;
        const uint32_t now = NowMs();
        const detail::Key held = HeldKey();
        detail::Key act = repeat.Update(held, now);
        if (const auto injected = static_cast<detail::Key>(g_injectedKey.exchange(0)); injected != detail::Key::None)
            act = injected;
        const bool funcDown = System::detail::HomeButtonRaw();
        if (funcWasDown && !funcDown)
        {
            if (!openingPress && act == detail::Key::None) act = detail::Key::Select;  // a tap on HOME chooses
            openingPress = false;
        }
        funcWasDown = funcDown;
        const detail::Outcome outcome = detail::Apply(scene.view.state, act);
        const bool backHeld = held == detail::Key::Back;
        if (outcome.changed || moving || backHeld != scene.view.backHeld)
        {
            scene.view.backHeld = backHeld;
            if (!DrawMenu(scene, now)) ESP_LOGW(kTag, "system menu: a paint failed");
            // Until a frame shows the bar at rest on its row (the one drawn when it stops moving is that).
            moving = scene.slide.active(scene.now);
        }
        if (outcome.chosen)
        {
            chosen = outcome.item;
            break;
        }
    }

    if (chosen != detail::Item::Continue)
    {
        // HOME low at a restart is the ROM's download strap, and it would wake a sleep at once.
        if (System::detail::HomeButtonRaw())
        {
            scene.view.releasePrompt = true;
            (void)DrawMenu(scene, NowMs());
            while (System::detail::HomeButtonRaw())
            {
                vTaskDelay(pdMS_TO_TICKS(kMenuPollMs));
                if (scene.slide.active(scene.now)) (void)DrawMenu(scene, NowMs());  // the bar comes to rest
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        // The screen goes dark as the Launcher's does when it turns the board off (its power-off fade); a
        // bar still moving comes to rest on its row meanwhile.
        const uint32_t start = NowMs();
        for (uint32_t elapsed = 0; elapsed < launcher::kPowerOffFadeMs; elapsed = NowMs() - start)
        {
            // Its brightness setting counted down, each step on its duty curve, as it does.
            Display::detail::SetBacklightDuty(
                detail::LauncherBacklightDuty(launcher::powerOffBrightness(g_launcherBrightness, elapsed),
                                              Display::detail::kBacklightFullDuty),
                true);
            if (scene.slide.active(scene.now)) (void)DrawMenu(scene, NowMs());
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        Display::detail::SetBacklightDuty(0, true);
        switch (chosen)
        {
            case detail::Item::Launcher: Leave(Action::Launcher);
            case detail::Item::Restart: Leave(Action::Restart);
            case detail::Item::PowerOff: Leave(Action::PowerOff);
            case detail::Item::Download: Leave(Action::Download);
            case detail::Item::Continue: break;
        }
    }

    // Continue: the way it came, backwards. The right part fades back to the firmware's picture (its latest
    // frame) over the menu as it was last drawn (`now` stays: a bar still moving stays where it was) while
    // the backlight goes back to the firmware's; the menu's left part stays as the curtain, the firmware has
    // its screen and its keys again, and MenuTask takes the curtain back to the left. Without the firmware's
    // picture its next frame gives the whole screen back.
    int curtain = 0;
    if (const uint16_t* picture = FirmwarePicture(); picture != nullptr)
    {
        curtain = detail::kPullRestPixels;
        scene.firmware = picture;
        scene.edge = curtain;
        Fade(detail::kCloseMs, [&](float part) {
            scene.mix = Between(256, 0, part);
            Display::detail::SetBacklightDuty(Between(menuDuty, firmwareDuty, part), true);
            (void)Display::detail::PaintOverlay(PaintScene, &scene);
        });
    }
    Display::detail::EndOverlay(curtain);
    ESP_LOGI(kTag, "system menu: closed");
    close(curtain);
}

// The curtain for this frame (detail::Curtain: the pull while HOME is held, the way back when it is let go).
// Without the firmware's picture nothing can give its columns back, so the curtain only goes out while HOME
// is held and is gone at once when it is let go (the firmware's next frame is the screen again).
void StepCurtain(detail::Curtain& curtain, const detail::HoldDetector& hold, uint32_t now)
{
    const bool pulling = hold.Pressing();
    float edge = curtain.Update(pulling, pulling ? hold.HeldMs(now) : 0, now);
    if (g_curtain > 0 && FirmwarePicture() == nullptr)
    {
        if (pulling) edge = std::max(edge, float(g_curtain));
        else
        {
            edge = 0;
            curtain.Reset();
        }
    }
    DrawCurtain(int(std::lround(edge)));
}

void MenuTask(void*)
{
    detail::HoldDetector hold;
    hold.Reset(System::detail::HomeButtonRaw());
    detail::Curtain curtain;
    while (true)
    {
        // Frames while the curtain is out or moving, the side key every kPollMs otherwise.
        vTaskDelay(pdMS_TO_TICKS(g_curtain > 0 || hold.Pressing() || curtain.Moving(NowMs()) ? kCurtainFrameMs : kPollMs));
        // Another driver may have reset GPIO0 (ESP-IDF's SD driver does, storage/storage.cpp): an input
        // turned off reads as always pressed. Give it back its input.
        if (!(REG_READ(IO_MUX_GPIO0_REG) & FUN_IE))
        {
            ESP_LOGW(kTag, "system menu: GPIO0's input was off (another driver reset it): restored");
            System::detail::RestoreHomeButton();
            hold.Reset(System::detail::HomeButtonRaw());
        }
        const bool down = System::detail::HomeButtonRaw();
        const uint32_t now = NowMs();
        if (!g_enabled.load())
        {
            hold.Reset(down);
            curtain.Reset();
            DrawCurtain(0);
            continue;
        }
        const bool fired = hold.Update(down, now);
        if (!fired)
        {
            StepCurtain(curtain, hold, now);
            continue;
        }
        RunMenu();
        // Continue leaves the menu's left part on the screen: back to the left from there.
        curtain.Reset();
        if (g_curtain > 0) curtain.ReturnFrom(float(g_curtain), NowMs());
        hold.Reset(System::detail::HomeButtonRaw());
    }
}

}  // namespace

namespace detail {

void InjectKey(Key key)
{
    g_injectedKey.store(static_cast<uint8_t>(key));
}

int CurtainEdge()
{
    return g_curtain;
}

bool HasFirmwarePicture()
{
    return FirmwarePicture() != nullptr;
}

uint32_t StackFree()
{
    return g_task != nullptr ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(g_task)) : 0;
}

}  // namespace detail

void Start()
{
    if (g_started.exchange(true)) return;
    if (xTaskCreate(MenuTask, "sys_menu", kStackBytes, nullptr, kPriority, &g_task) != pdPASS)
    {
        ESP_LOGE(kTag, "system menu: no task");
        g_started.store(false);
    }
}

void Enable(bool enabled)
{
    g_enabled.store(enabled);
}

bool Enabled()
{
    return g_enabled.load();
}

bool IsOpen()
{
    return g_open.load();
}

void SetListener(Listener listener, void* context)
{
    portENTER_CRITICAL(&g_hooksMux);
    g_listener = listener;
    g_listenerContext = context;
    portEXIT_CRITICAL(&g_hooksMux);
}

void SetLeaveHook(LeaveHook hook, void* context)
{
    portENTER_CRITICAL(&g_hooksMux);
    g_leaveHook = hook;
    g_leaveContext = context;
    portEXIT_CRITICAL(&g_hooksMux);
}

}  // namespace CardenzaHAL::SystemMenu
