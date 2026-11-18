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
// The Launcher's text code works with String and glyph buffers on the stack.
constexpr uint32_t kStackBytes = 5120;
// The menu at least this bright (0-255) while it shows, whatever the firmware's brightness.
constexpr int kMenuBacklight = 128;
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

// ---- The Launcher's language: NVS launcher/lang, read page by page without the NVS library ----------

int ReadLauncherLanguage()
{
    const esp_partition_t* nvs =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
    if (nvs == nullptr) return 0;
    auto* page = static_cast<uint8_t*>(heap_caps_malloc(detail::kNvsPageBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (page == nullptr) page = static_cast<uint8_t*>(heap_caps_malloc(detail::kNvsPageBytes, MALLOC_CAP_8BIT));
    if (page == nullptr) return 0;
    const std::size_t pages = nvs->size / detail::kNvsPageBytes;
    uint8_t index = 0;
    for (std::size_t i = 0; i < pages && index == 0; ++i)
        if (esp_partition_read(nvs, i * detail::kNvsPageBytes, page, detail::kNvsPageBytes) == ESP_OK)
            index = detail::NvsNamespaceIndex(page, "launcher");
    uint8_t lang = 0;
    bool found = false;
    for (std::size_t i = 0; i < pages && index != 0 && !found; ++i)
        if (esp_partition_read(nvs, i * detail::kNvsPageBytes, page, detail::kNvsPageBytes) == ESP_OK)
            found = detail::NvsFindU8(page, index, "lang", lang);
    heap_caps_free(page);
    return found ? lang : 0;
}

// ---- The picture ---------------------------------------------------------------------------------------

// What the screen shows: the menu (drawn by the Launcher's code, frame.cpp), slid by `offset` while it is a
// curtain, and right of `edge` the firmware's picture (its last frame, captured) mixed with it.
struct Scene {
    detail::View view;
    launcher::ListSlide slide;          // the selection bar's motion, once the menu is open
    bool useSlide = false;
    uint32_t now = 0;                   // the menu's frame is drawn at this time
    int offset = 0;                     // the menu's column c is at x = c + offset (left of it: the rail)
    int edge = detail::kWidth;          // from here on: firmware and menu mixed
    int mix = 256;                      // 0 the firmware's picture, 256 the menu
    const uint16_t* firmware = nullptr; // null: no picture (right of edge is the menu)
    // The menu's picture, drawn once a frame (PSRAM): the pull and the pop show the one Prepare drew (the
    // first row selected) as they move it every 10 ms, and drawing the Launcher's frame again for every
    // band of every step kept the CPU so busy that the firmware's own tasks below this one stalled (the
    // board's self-test, 2026-11-18); in the menu each frame of the selection bar's slide is drawn into it
    // (DrawMenu). Null without PSRAM: every band draws the frame (the same `now`, the same picture).
    uint8_t* still = nullptr;
    uint8_t grey[detail::kWidth * 8];   // one band of the menu
};

Scene* g_scene = nullptr;
uint16_t* g_capture = nullptr;  // the firmware's frame, PSRAM (no PSRAM: no fades over its picture)
int g_curtain = 0;              // the curtain's edge on the screen
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
            const int px = x + c, menuX = px - scene.offset;
            const uint8_t grey = menuX < 0 ? 0 : menuX >= detail::kWidth ? 255 : menu[r * detail::kWidth + menuX];
            uint16_t pixel = detail::PanelGrey(grey);
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
    g_scene->view.language = detail::LanguageIndex(ReadLauncherLanguage());
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

// The curtain to `edge` pixels: the menu slides with its edge (only that strip is sent: the panel has no
// tearing-effect line, and a narrow strip shows none), the firmware keeps the rest of the screen.
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
    scene.offset = detail::ClothOffset(edge);
    scene.edge = edge;
    scene.mix = 0;
    scene.firmware = FirmwarePicture();
    if (edge > g_curtain)
    {
        Display::detail::SetCurtain(edge, false);
        (void)Display::detail::PaintColumns(PaintScene, &scene, 0, edge);
    }
    else
    {
        // Going back: the columns it leaves show the firmware's picture (its last frame) at once when there
        // is one; else its next frame, sent whole, gives them back.
        const bool picture = scene.firmware != nullptr;
        (void)Display::detail::PaintColumns(PaintScene, &scene, 0, picture ? g_curtain : edge);
        Display::detail::SetCurtain(edge, !picture);
    }
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
    const auto key = static_cast<gpio_num_t>(Pins::kFuncButton);
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
    System::detail::HoldFuncButton(true);
    g_open.store(true);
    Notify(true);
    Keyboard::LedFrame lights{};
    for (const auto& key : detail::kNavigationKeys)
        lights[static_cast<std::size_t>(key.y) * Keyboard::LED_COLUMNS + key.x] = Keyboard::LED_BRIGHTNESS_MAX;
    Keyboard::detail::BeginOverlay(lights);
    const auto close = [&] {
        Keyboard::detail::EndOverlay();
        g_open.store(false);
        g_curtain = 0;
        Release();
        System::detail::HoldFuncButton(false);
        Notify(false);
    };
    if (!Display::detail::BeginOverlay())
    {
        // No screen to show it on (the firmware never set the display up): nothing to choose from.
        ESP_LOGE(kTag, "system menu: no display");
        Display::detail::SetCurtain(0, true);
        close();
        return;
    }

    // The pop: the rest of the menu fades in over the firmware's picture (the curtain is already the menu's
    // left part, where it will stay); without its picture the curtain opens the rest quickly instead.
    const int firmwareBacklight = Display::GetBrightness();
    const int menuBacklight = std::max(firmwareBacklight, kMenuBacklight);
    scene.offset = 0;
    scene.firmware = FirmwarePicture();
    if (scene.firmware != nullptr)
    {
        scene.edge = g_curtain;
        Fade(detail::kPopMs, [&](float part) {
            scene.now = NowMs();
            scene.mix = Between(0, 256, part);
            Display::detail::SetBacklight(Between(firmwareBacklight, menuBacklight, part));
            (void)Display::detail::PaintOverlay(PaintScene, &scene);
        });
    }
    else
    {
        scene.edge = detail::kWidth;
        const int from = g_curtain;
        Fade(detail::kPopMs, [&](float part) {
            scene.now = NowMs();
            Display::detail::SetBacklight(Between(firmwareBacklight, menuBacklight, part));
            (void)Display::detail::PaintColumns(PaintScene, &scene, 0, Between(from, detail::kWidth, part));
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
        const bool funcDown = System::detail::FuncButtonRaw();
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
        if (System::detail::FuncButtonRaw())
        {
            scene.view.releasePrompt = true;
            (void)DrawMenu(scene, NowMs());
            while (System::detail::FuncButtonRaw())
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
            Display::detail::SetBacklight(menuBacklight * launcher::powerOffBrightness(100, elapsed) / 100);
            if (scene.slide.active(scene.now)) (void)DrawMenu(scene, NowMs());
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        Display::detail::SetBacklight(0);
        switch (chosen)
        {
            case detail::Item::Launcher: Leave(Action::Launcher);
            case detail::Item::Restart: Leave(Action::Restart);
            case detail::Item::PowerOff: Leave(Action::PowerOff);
            case detail::Item::Download: Leave(Action::Download);
            case detail::Item::Continue: break;
        }
    }

    // Continue: the firmware's picture (its latest frame) fades back in over the menu as it was last drawn
    // (`now` stays: a bar still moving stays where it was), then its own frames go on.
    if (const uint16_t* picture = FirmwarePicture(); picture != nullptr)
    {
        scene.firmware = picture;
        scene.edge = 0;
        Fade(detail::kCloseMs, [&](float part) {
            scene.mix = Between(256, 0, part);
            Display::detail::SetBacklight(Between(menuBacklight, firmwareBacklight, part));
            (void)Display::detail::PaintOverlay(PaintScene, &scene);
        });
    }
    Display::detail::EndOverlay();
    ESP_LOGI(kTag, "system menu: closed");
    close();
}

// HOME's press: while it can still open the menu the curtain follows it; let go, it goes back (the
// Launcher's return), and a new press takes it from where it is.
struct Curtain {
    bool returning = false;
    float from = 0;
    uint32_t releasedAt = 0;
    float shown = 0;

    void Update(const detail::HoldDetector& hold, uint32_t now)
    {
        float edge = 0;
        if (returning)
        {
            const uint32_t since = now - releasedAt;
            edge = since >= launcher::kPowerReturnMs ? 0.f : detail::ReturnEdge(from, since);
            if (since >= launcher::kPowerReturnMs) returning = false;
        }
        if (hold.Pressing()) edge = std::max(edge, detail::PullEdge(hold.HeldMs(now)));
        else if (!returning && shown > 0)
        {
            returning = true;
            from = shown;
            releasedAt = now;
            edge = shown;
        }
        shown = edge;
        DrawCurtain(int(std::lround(edge)));
    }
    void Reset()
    {
        returning = false;
        shown = 0;
    }
};

void MenuTask(void*)
{
    detail::HoldDetector hold;
    hold.Reset(System::detail::FuncButtonRaw());
    Curtain curtain;
    while (true)
    {
        // Frames while the curtain moves, the side key every kPollMs otherwise.
        vTaskDelay(pdMS_TO_TICKS(curtain.shown > 0 || hold.Pressing() ? kMenuPollMs : kPollMs));
        // Another driver may have reset GPIO0 (ESP-IDF's SD driver does, storage/storage.cpp): an input
        // turned off reads as always pressed. Give it back its input.
        if (!(REG_READ(IO_MUX_GPIO0_REG) & FUN_IE))
        {
            ESP_LOGW(kTag, "system menu: GPIO0's input was off (another driver reset it): restored");
            System::detail::RestoreFuncButton();
            hold.Reset(System::detail::FuncButtonRaw());
        }
        const bool down = System::detail::FuncButtonRaw();
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
            curtain.Update(hold, now);
            continue;
        }
        RunMenu();
        curtain.Reset();
        hold.Reset(System::detail::FuncButtonRaw());
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
