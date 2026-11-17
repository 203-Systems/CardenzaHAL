// The system menu (system/menu.h): long-press HOME in any firmware on the HAL.
#include "system/menu.h"

#include <atomic>
#include <cstdio>
#include <cstring>

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
#include "display/internal/overlay.h"
#include "keyboard/internal/overlay.h"
#include "system/internal/hooks.h"
#include "system/launcher.h"
#include "system/menu/logic.h"
#include "system/menu/test.h"

namespace CardenzaHAL::SystemMenu {

namespace {

constexpr char kTag[] = "CardenzaHAL";
constexpr uint32_t kPollMs = 20;      // the side key, while the menu is closed
constexpr uint32_t kMenuPollMs = 10;  // keys while it is open
constexpr uint32_t kStackBytes = 3072;
// Above a firmware's UI loop (usually 1 to 3), below audio and USB: the menu answers while the firmware
// is busy drawing, and the firmware's sound keeps going.
constexpr UBaseType_t kPriority = 4;

std::atomic<bool> g_started{false};
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

void PaintBand(void* context, int y, int rows, uint16_t* band)
{
    detail::Paint(*static_cast<const detail::View*>(context), y, rows, band);
}

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
    static char firmware[80];
    const esp_app_desc_t* app = esp_app_get_description();
    std::snprintf(firmware, sizeof(firmware), "%.32s %.32s", app->project_name, app->version);
    static detail::View view;
    view = detail::View{};
    view.language = detail::LanguageIndex(ReadLauncherLanguage());
    view.firmware = firmware;

    ESP_LOGI(kTag, "system menu: open");
    System::detail::HoldFuncButton(true);
    g_open.store(true);
    Notify(true);
    Keyboard::LedFrame lights{};
    for (const auto& key : detail::kNavigationKeys)
        lights[static_cast<std::size_t>(key.y) * Keyboard::LED_COLUMNS + key.x] = Keyboard::LED_BRIGHTNESS_MAX;
    Keyboard::detail::BeginOverlay(lights);
    if (!Display::detail::BeginOverlay())
    {
        // No screen to show it on (the firmware never set the display up): nothing to choose from.
        ESP_LOGE(kTag, "system menu: no display");
        Keyboard::detail::EndOverlay();
        g_open.store(false);
        System::detail::HoldFuncButton(false);
        Notify(false);
        return;
    }
    if (!Display::detail::PaintOverlay(PaintBand, &view)) ESP_LOGW(kTag, "system menu: a paint failed");

    detail::KeyRepeat repeat;
    bool openingPress = true;  // the press that opened the menu: its release is no tap
    bool funcWasDown = true;
    detail::Item chosen = detail::Item::Continue;
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(kMenuPollMs));
        if (!g_enabled.load()) break;
        detail::Key act = repeat.Update(HeldKey(), NowMs());
        if (const auto injected = static_cast<detail::Key>(g_injectedKey.exchange(0)); injected != detail::Key::None)
            act = injected;
        const bool funcDown = System::detail::FuncButtonRaw();
        if (funcWasDown && !funcDown)
        {
            if (!openingPress && act == detail::Key::None) act = detail::Key::Select;  // a tap on HOME chooses
            openingPress = false;
        }
        funcWasDown = funcDown;
        const detail::Outcome outcome = detail::Apply(view.state, act);
        if (outcome.changed && !Display::detail::PaintOverlay(PaintBand, &view))
            ESP_LOGW(kTag, "system menu: a paint failed");
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
            view.releasePrompt = true;
            (void)Display::detail::PaintOverlay(PaintBand, &view);
            while (System::detail::FuncButtonRaw()) vTaskDelay(pdMS_TO_TICKS(kMenuPollMs));
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        switch (chosen)
        {
            case detail::Item::Launcher: Leave(Action::Launcher);
            case detail::Item::Restart: Leave(Action::Restart);
            case detail::Item::PowerOff: Leave(Action::PowerOff);
            case detail::Item::Download: Leave(Action::Download);
            case detail::Item::Continue: break;
        }
    }

    Display::detail::EndOverlay();
    Keyboard::detail::EndOverlay();
    g_open.store(false);
    System::detail::HoldFuncButton(false);
    ESP_LOGI(kTag, "system menu: closed");
    Notify(false);
}

void MenuTask(void*)
{
    detail::HoldDetector hold;
    hold.Reset(System::detail::FuncButtonRaw());
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
        // Another driver may have reset GPIO0 (ESP-IDF's SD driver does, storage/storage.cpp): an input
        // turned off reads as always pressed. Give it back its input.
        if (!(REG_READ(IO_MUX_GPIO0_REG) & FUN_IE))
        {
            ESP_LOGW(kTag, "system menu: GPIO0's input was off (another driver reset it): restored");
            System::detail::RestoreFuncButton();
            hold.Reset(System::detail::FuncButtonRaw());
        }
        const bool down = System::detail::FuncButtonRaw();
        if (!g_enabled.load())
        {
            hold.Reset(down);
            continue;
        }
        if (!hold.Update(down, NowMs())) continue;
        RunMenu();
        hold.Reset(System::detail::FuncButtonRaw());
    }
}

}  // namespace

namespace detail {

void InjectKey(Key key)
{
    g_injectedKey.store(static_cast<uint8_t>(key));
}

}  // namespace detail

void Start()
{
    if (g_started.exchange(true)) return;
    if (xTaskCreate(MenuTask, "sys_menu", kStackBytes, nullptr, kPriority, nullptr) != pdPASS)
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
