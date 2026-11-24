// Back to the 203 Launcher after early crashes in a row (system/crash_loop_logic.h, Kconfig
// CONFIG_CARDENZA_HAL_CRASH_LOOP_LAUNCHER).
#include "system/crash_loop_logic.h"
#include "system/internal/hooks.h"
#include "system/launcher.h"

#include <esp_attr.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <sdkconfig.h>

namespace CardenzaHAL::System::detail {

#if CONFIG_CARDENZA_HAL_CRASH_LOOP_LAUNCHER
namespace {

RTC_NOINIT_ATTR CrashLoop::State g_crashLoop;
esp_timer_handle_t g_lasted = nullptr;

CrashLoop::Reset Classify(esp_reset_reason_t reason)
{
    switch (reason)
    {
        case ESP_RST_POWERON: return CrashLoop::Reset::PowerOn;
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT: return CrashLoop::Reset::Crash;
        default: return CrashLoop::Reset::Other;
    }
}

}  // namespace

void CheckCrashLoop()
{
    static bool checked = false;  // once a start, however often Init is retried
    if (checked) return;
    checked = true;
    const uint32_t crashes = CrashLoop::Count(g_crashLoop, Classify(esp_reset_reason()));
    g_crashLoop.magic = CrashLoop::kMagic;
    g_crashLoop.crashes = crashes;
    if (CrashLoop::GiveUp(crashes, CONFIG_CARDENZA_HAL_CRASH_LOOP_CRASHES))
    {
        g_crashLoop.crashes = 0;
        ESP_LOGE("CardenzaHAL", "crashed %u times in a row within %d s of starting: back to the Launcher", unsigned(crashes),
                 CONFIG_CARDENZA_HAL_CRASH_LOOP_SECONDS);
        System::RestartIntoLauncher();
    }
    if (crashes != 0)
        ESP_LOGW("CardenzaHAL", "crash %u of %d in a row at start (then the Launcher)", unsigned(crashes), CONFIG_CARDENZA_HAL_CRASH_LOOP_CRASHES);
    // A run that lasts clears the count.
    if (g_lasted == nullptr)
    {
        esp_timer_create_args_t args = {};
        args.callback = [](void*) { g_crashLoop.crashes = 0; };
        args.name = "crash_loop";
        if (esp_timer_create(&args, &g_lasted) == ESP_OK)
            esp_timer_start_once(g_lasted, static_cast<uint64_t>(CONFIG_CARDENZA_HAL_CRASH_LOOP_SECONDS) * 1000000u);
    }
}
#else
void CheckCrashLoop() {}
#endif

}  // namespace CardenzaHAL::System::detail
