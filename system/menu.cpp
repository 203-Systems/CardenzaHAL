// The system menu (system/menu.h): a press of HOME in any firmware on the HAL.
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
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <soc/io_mux_reg.h>
#include <spi_flash_mmap.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/rtc_cntl_struct.h>
#include <sdkconfig.h>
#if CONFIG_SPIRAM
#include <esp_psram.h>
#endif
#include <esp_vfs_fat.h>

#include "board/pins.h"
#include "display/backlight_brightness.h"
#include "display/display.h"
#include "display/internal/overlay.h"
#include "keyboard/internal/flash_guard.h"
#include "keyboard/internal/overlay.h"
#include "system/internal/hooks.h"
#include "system/launcher.h"
#include "system/menu/frame.h"
#include "system/menu/launcher/motion.h"
#include "system/menu/logic.h"
#include "system/menu/test.h"
#include "system/internal/settings_hooks.h"
#include "system/settings.h"
#include "storage/storage.h"

namespace CardenzaHAL::SystemMenu {

namespace {

constexpr char kTag[] = "CardenzaHAL";
constexpr uint32_t kPollMs = 10;      // the side key, while the menu is closed
constexpr uint32_t kMenuPollMs = 10;  // keys while it is open
// After the press, the firmware's next frame is waited for this long at most: the backdrop is made from it.
constexpr uint32_t kPictureWaitMs = 60;
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

// Set by CMakeLists.txt when the build is configured (the About page's HAL row).
#ifndef CARDENZA_HAL_COMMIT
#define CARDENZA_HAL_COMMIT ""
#endif
#ifndef CARDENZA_HAL_DIRTY
#define CARDENZA_HAL_DIRTY 0
#endif

// ---- The picture ---------------------------------------------------------------------------------------

// What the screen shows: the backdrop (the firmware's last frame, captured, dark and grey through a dither;
// detail::BackdropGrey), the menu (drawn by the Launcher's code, frame.cpp), or at a step of the dissolve
// each pixel one or the other; when the menu closes, the firmware's picture as it is. Every pixel but that
// last picture's is a neutral grey (detail::PanelGrey).
struct Scene {
    detail::View view;
    launcher::ListSlide slide;          // the selection bar's motion, once the menu is open
    bool useSlide = false;
    uint32_t now = 0;                   // the menu's frame is drawn at this time
    int step = 0;                       // 0 the backdrop, detail::kDissolveSteps the menu
    bool picture = false;               // the firmware's picture as it is (closing)
    const uint16_t* firmware = nullptr; // null: no picture (the backdrop is a dark field)
    // The menu's picture, drawn once a frame (PSRAM): drawing the Launcher's frame again for every band
    // kept the CPU so busy that the firmware's own tasks below this one stalled (the board's self-test,
    // 2026-11-18); in the menu each frame of the selection bar's slide is drawn into it (DrawMenu). Null
    // without PSRAM: every band draws the frame (the same `now`, the same picture).
    uint8_t* still = nullptr;
    uint8_t grey[detail::kWidth * 8];   // one band of the menu
};

Scene* g_scene = nullptr;
uint16_t* g_capture = nullptr;  // the firmware's frame, PSRAM (no PSRAM: the backdrop is a dark field)
// The backlight's duty while the menu shows: the board's screen brightness (the Launcher's own setting, on
// its curve).
int g_brightness = 100;  // percent
int g_menuDuty = Display::detail::kBacklightFullDuty;
char g_firmwareName[80];
detail::AboutInfo g_about;

// The settings as the menu shows them.
Settings::detail::Values SettingsNow()
{
    Settings::detail::Values values{};
    for (int i = 0; i < Settings::kCount; ++i) values[i] = Settings::Get(Settings::detail::IdAt(i));
    return values;
}

void PaintScene(void* context, int x, int y, int width, int rows, uint16_t* out)
{
    Scene& scene = *static_cast<Scene*>(context);
    if (scene.picture && scene.firmware != nullptr)
    {
        for (int r = 0; r < rows; ++r)
            std::memcpy(out + r * width, scene.firmware + (y + r) * detail::kWidth + x, std::size_t(width) * sizeof(uint16_t));
        return;
    }
    const uint8_t* menu = scene.grey;
    if (scene.step > 0)
    {
        if (scene.still != nullptr) menu = scene.still + y * detail::kWidth;
        else detail::PaintGrey(scene.view, scene.useSlide ? &scene.slide : nullptr, scene.now, y, rows, scene.grey);
    }
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < width; ++c)
        {
            const int px = x + c, py = y + r;
            const uint8_t menuGrey = scene.step > 0 ? menu[r * detail::kWidth + px] : 0;
            out[r * width + c] = detail::PanelGrey(detail::SceneGrey(
                scene.step, menuGrey, scene.firmware != nullptr ? scene.firmware + py * detail::kWidth + px : nullptr, px, py));
        }
}

// The scene for the menu: the Launcher's language, the firmware's name, the first row selected;
// and a buffer the firmware's frames are copied into (the backdrop is made from its picture).
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
    g_scene->view.state.values = SettingsNow();
    g_brightness = Settings::Get(Settings::Id::ScreenBrightness);
    g_menuDuty = detail::LauncherBacklightDuty(g_brightness, Display::detail::kBacklightFullDuty);
    g_scene->view.language = detail::LanguageIndex(Settings::Get(Settings::Id::Language));
    g_scene->view.firmware = g_firmwareName;
    g_scene->view.about = &g_about;
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

// ---- What the board is (the About page) ----------------------------------------------------------------

// The Launcher's own image description, at its slot (0x10000, whatever the table calls it), as the
// development channel reads it (usb/dev.cpp): through the cache, never with it disabled.
bool LauncherVersion(char* out, std::size_t size)
{
    constexpr uint32_t kLauncherSlot = 0x10000;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running != nullptr && running->address == kLauncherSlot) return false;
    const void* mapped = nullptr;
    spi_flash_mmap_handle_t handle = 0;
    if (spi_flash_mmap(kLauncherSlot, 0x10000, SPI_FLASH_MMAP_DATA, &mapped, &handle) != ESP_OK) return false;
    const auto* bytes = static_cast<const uint8_t*>(mapped);
    esp_app_desc_t desc;
    // The image header (24 bytes) and the first segment's header (8): then the description.
    std::memcpy(&desc, bytes + 32, sizeof(desc));
    const bool image = bytes[0] == 0xE9;
    spi_flash_munmap(handle);
    if (!image || desc.magic_word != ESP_APP_DESC_MAGIC_WORD) return false;
    // The Launcher's version is "<version>-<board>": the version alone, as its About shows it.
    std::snprintf(out, size, "%.*s", int(sizeof(desc.version)), desc.version);
    if (char* board = std::strchr(out, '-')) *board = 0;
    return out[0] != 0;
}

void GatherAbout(detail::AboutInfo& about)
{
    about = detail::AboutInfo{};
    const esp_app_desc_t* app = esp_app_get_description();
    std::snprintf(about.firmware, sizeof(about.firmware), "%.32s", app->project_name);
    std::snprintf(about.version, sizeof(about.version), "%.32s", app->version);
    if (!LauncherVersion(about.launcher, sizeof(about.launcher))) about.launcher[0] = 0;
    std::snprintf(about.hal, sizeof(about.hal), "%.16s%s", CARDENZA_HAL_COMMIT, CARDENZA_HAL_DIRTY ? "+" : "");
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK)
        std::snprintf(about.mac, sizeof(about.mac), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
                      mac[4], mac[5]);
    uint32_t flash = 0;
    if (esp_flash_get_size(nullptr, &flash) == ESP_OK) about.flashBytes = flash;
#if CONFIG_SPIRAM
    about.psramBytes = static_cast<uint32_t>(esp_psram_get_size());
#endif
    if (Storage::IsMounted())
    {
        uint64_t total = 0, free = 0;
        if (esp_vfs_fat_info(Storage::kMountPoint, &total, &free) == ESP_OK)
        {
            about.card = true;
            about.cardBytes = total;
            about.cardFree = free;
        }
    }
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
    // What changed in the menu is in the flash before the board goes.
    (void)Settings::Flush();
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
    const auto close = [&] {
        Keyboard::detail::EndOverlay();
        g_open.store(false);
        Release();
        System::detail::HoldHomeButton(false);
        Notify(false);
    };
    if (!Display::detail::BeginOverlay())
    {
        // No screen to show it on (the firmware never set the display up): nothing to choose from.
        ESP_LOGE(kTag, "system menu: no display");
        close();
        return;
    }

    // The backdrop: the firmware's picture, dark and grey through the dither, in one frame. Its frames are
    // copied since HOME went down (MenuTask); one that is still on its way is waited for a moment.
    const int firmwareDuty = Display::detail::FirmwareBacklightDuty();
    const int menuDuty = g_menuDuty;
    for (const uint32_t since = NowMs(); g_capture != nullptr && FirmwarePicture() == nullptr && NowMs() - since < kPictureWaitMs;)
        vTaskDelay(pdMS_TO_TICKS(2));
    scene.firmware = FirmwarePicture();
    scene.picture = false;
    scene.step = 0;
    scene.now = NowMs();
    const uint32_t backdropAt = scene.now;
    if (!Display::detail::PaintOverlay(PaintScene, &scene)) ESP_LOGW(kTag, "system menu: a paint failed");
    while (NowMs() - backdropAt < detail::kBackdropMs) vTaskDelay(1);
    // The menu comes through it in the dither's order, a frame a step (each pixel the backdrop's or the
    // menu's: greys only), while the backlight goes to the Launcher's, on the Launcher's PWM.
    for (int step = 1; step < detail::kDissolveSteps; ++step)
    {
        scene.step = step;
        Display::detail::SetBacklightDuty(firmwareDuty + (menuDuty - firmwareDuty) * step / detail::kDissolveSteps, true);
        (void)Display::detail::PaintOverlay(PaintScene, &scene);
    }
    scene.step = detail::kDissolveSteps;
    Display::detail::SetBacklightDuty(menuDuty, true);
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
        // A setting changed from elsewhere meanwhile (USB, the firmware): shown as it is.
        bool external = false;
        if (const auto values = SettingsNow(); values != scene.view.state.values)
        {
            scene.view.state.values = values;
            external = true;
        }
        const detail::Outcome outcome = detail::Apply(scene.view.state, act);
        if (outcome.set) (void)Settings::Set(outcome.id, outcome.value);
        if (outcome.reset) Settings::Reset();
        if (outcome.set || outcome.reset || external)
        {
            // Live: the volume is heard and the key lights seen at once (Settings applies them); the screen
            // while the menu shows is its own backlight duty.
            scene.view.state.values = SettingsNow();
            const int brightness = scene.view.state.values[Settings::detail::Index(Settings::Id::ScreenBrightness)];
            if (brightness != g_brightness)
            {
                g_brightness = brightness;
                g_menuDuty = detail::LauncherBacklightDuty(brightness, Display::detail::kBacklightFullDuty);
                Display::detail::SetBacklightDuty(g_menuDuty, true);
            }
            scene.view.language = detail::LanguageIndex(scene.view.state.values[Settings::detail::Index(Settings::Id::Language)]);
        }
        if (outcome.page)
        {
            // Another page: drawn at once, a new selection bar placed on its row (the panel has no
            // tearing-effect line: pages change in one frame, as the Launcher's do on the Cardenza).
            scene.slide = launcher::ListSlide{};
            scene.slide.keep = true;
            if (scene.view.state.page == detail::Page::About) GatherAbout(g_about);
            // The key lights' master level shows as it is while it is being changed (no floor).
            const bool keyLights = scene.view.state.page == detail::Page::Value &&
                                   scene.view.state.editing == Settings::Id::KeyBrightness;
            Keyboard::detail::SetOverlayFloor(!keyLights);
        }
        const bool backHeld = held == detail::Key::Back;
        if (outcome.changed || external || moving || backHeld != scene.view.backHeld)
        {
            scene.view.backHeld = backHeld;
            if (!DrawMenu(scene, now)) ESP_LOGW(kTag, "system menu: a paint failed");
            // Until a frame shows the bar at rest on its row (the one drawn when it stops moving is that).
            moving = scene.slide.active(scene.now);
        }
        if (outcome.close) break;
        if (outcome.leave)
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
                detail::LauncherBacklightDuty(launcher::powerOffBrightness(g_brightness, elapsed),
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
            default: break;
        }
    }

    // Continue: the backdrop again (the firmware's latest frame) at the firmware's brightness, then its
    // picture as it is; its own frames go on from there (the next one whole).
    if (const uint16_t* picture = FirmwarePicture(); picture != nullptr)
    {
        scene.firmware = picture;
        scene.step = 0;
        // The firmware's light now: the screen brightness may have changed in the menu.
        Display::detail::SetBacklightDuty(Display::detail::FirmwareBacklightDuty(), true);
        (void)Display::detail::PaintOverlay(PaintScene, &scene);
        vTaskDelay(pdMS_TO_TICKS(detail::kCloseMs));
        scene.picture = true;
        (void)Display::detail::PaintOverlay(PaintScene, &scene);
    }
    Display::detail::EndOverlay();
    ESP_LOGI(kTag, "system menu: closed");
    close();
    // What changed while it was open is written now (once, on the settings' own task).
    Settings::detail::MenuClosed();
}

void MenuTask(void*)
{
    detail::PressDetector press;
    press.Reset(System::detail::HomeButtonRaw());
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
        // Another driver may have reset GPIO0 (ESP-IDF's SD driver does, storage/storage.cpp): an input
        // turned off reads as always pressed. Give it back its input.
        if (!(REG_READ(IO_MUX_GPIO0_REG) & FUN_IE))
        {
            ESP_LOGW(kTag, "system menu: GPIO0's input was off (another driver reset it): restored");
            System::detail::RestoreHomeButton();
            press.Reset(System::detail::HomeButtonRaw());
        }
        const bool down = System::detail::HomeButtonRaw();
        if (!g_enabled.load())
        {
            press.Reset(down);
            if (g_scene != nullptr) Release();
            continue;
        }
        const bool open = press.Update(down, NowMs());
        // HOME just went down on a press that will open the menu: the menu's picture is drawn and the
        // firmware's frames are copied from now, so its picture is there when the press has lasted.
        if (press.Pending() && g_scene == nullptr && Display::detail::Ready()) (void)Prepare();
        if (!down && g_scene != nullptr) Release();  // a bounce: nothing opened
        if (!open) continue;
        RunMenu();
        // A HOME still down (the menu was closed with a key) opens nothing until it has been released.
        press.Reset(System::detail::HomeButtonRaw());
    }
}

}  // namespace

namespace detail {

void InjectKey(Key key)
{
    g_injectedKey.store(static_cast<uint8_t>(key));
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
