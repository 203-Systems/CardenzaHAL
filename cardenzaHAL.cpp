#include "cardenzaHAL.h"

#include <esp_log.h>
#include <sdkconfig.h>

#include "net/presence.h"
#include "system/internal/hooks.h"
#include "system/internal/settings_hooks.h"
#include "usb/internal/start.h"
#include "system/menu.h"

namespace {

CardenzaHAL::InitReport g_initReport;
bool g_initialized = false;

constexpr char kLogTag[] = "CardenzaHAL";

void LogModuleResult(const char* moduleName, bool initialized, bool optional)
{
    if (initialized)
    {
        ESP_LOGI(kLogTag, "%s initialized", moduleName);
        return;
    }

    if (optional)
    {
        ESP_LOGW(kLogTag, "%s unavailable", moduleName);
    }
    else
    {
        ESP_LOGE(kLogTag, "%s failed", moduleName);
    }
}

}  // namespace

namespace CardenzaHAL {

bool Init()
{
    if (g_initialized)
    {
        return g_initReport.overall;
    }

    g_initReport = {};

    // A firmware that keeps crashing right after its start goes back to the Launcher (Kconfig
    // CONFIG_CARDENZA_HAL_CRASH_LOOP_LAUNCHER), before anything of it runs again.
    System::detail::CheckCrashLoop();

    g_initReport.system = System::Init();
    LogModuleResult("system", g_initReport.system, false);

    // The board's settings (system/settings.h) from the flash, before the drivers: the codec starts at the
    // volume, the backlight at the brightness, USB with its landing page or without.
    Settings::detail::Load();

#if CONFIG_CARDENZA_HAL_USB_AUTOSTART
    // USB before the drivers (Kconfig CONFIG_CARDENZA_HAL_USB_AUTOSTART): whatever else fails to start, the board
    // stays reachable, and recognised and flashed over USB with no key pressed. It connects a second later,
    // unless the firmware's own USB::Init takes it over first.
    USB::detail::AutoStart();
#endif

    if (!g_initReport.system)
    {
        g_initReport.overall = false;
        return g_initReport.overall;
    }

    g_initReport.keyboard = Keyboard::init();
    LogModuleResult("keyboard", g_initReport.keyboard, false);

    g_initReport.audio = Audio::Init();
    LogModuleResult("audio", g_initReport.audio, false);

    g_initReport.display = Display::Init();
    LogModuleResult("display", g_initReport.display, false);

    g_initReport.storage = Storage::Init();
    LogModuleResult("storage", g_initReport.storage, true);

    g_initReport.ir = IR::Init();
    LogModuleResult("ir", g_initReport.ir, true);

    Settings::detail::ApplyAfterInit();

    g_initReport.overall = g_initReport.system && g_initReport.keyboard && g_initReport.audio && g_initReport.display;
    g_initialized = g_initReport.overall;
#if CONFIG_CARDENZA_HAL_SYSTEM_MENU
    // A press of HOME: the system menu (back to the Launcher, restart, turn off; system/menu.h).
    if (g_initReport.display) SystemMenu::Start();
#endif
#if CONFIG_CARDENZA_HAL_PRESENCE
    // Reachable from the community while this firmware runs (net/presence.h); it starts later, and only
    // on a board the Launcher bound and connected. Start() runs once however often Init is retried.
    Presence::Start();
#endif
    return g_initReport.overall;
}

const InitReport& LastInitReport()
{
    return g_initReport;
}

}  // namespace CardenzaHAL
