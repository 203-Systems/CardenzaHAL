// Restarting into the 203 Launcher (system/launcher.h).
#include "system/launcher.h"
#include "system/internal/hooks.h"

#include <esp_log.h>
#include <esp_newlib.h>
#include <esp_system.h>
#include <soc/rtc_cntl_struct.h>

namespace CardenzaHAL::System {

void RestartIntoLauncher()
{
    ESP_LOGW("CardenzaHAL", "restarting into the Launcher");
    // The top of RTC fast memory is ESP-IDF's reserved segment in every app (its RTC timer data, and the
    // bootloader's data in the Launcher's own build): never heap. The firmware is going away; what it
    // kept there does not matter any more.
    detail::RestartWithTicket(LauncherTicket::kRestart);
}

namespace detail {

void WriteLauncherTicket(uint32_t value)
{
    volatile uint32_t* ticket = reinterpret_cast<volatile uint32_t*>(LauncherTicket::kAddress);
    ticket[0] = value;
    ticket[1] = LauncherTicket::Inverse(value);
}

void RestartWithTicket(uint32_t value)
{
    // In a firmware built without the Launcher's CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC, ESP-IDF keeps its RTC
    // timer's retained time (s_rtc_timer_retain_mem, 24 bytes) exactly where the Launcher's bootloader reads
    // the ticket, and esp_timer's shutdown handler (esp_sync_timekeeping_timers) writes it during
    // esp_restart: a ticket written before it is lost (seen on the board 2026-11-17: Restart went through
    // the Launcher's countdown). So: sync the time now, take that handler out, write the ticket last.
    esp_sync_timekeeping_timers();
    (void)esp_unregister_shutdown_handler(esp_sync_timekeeping_timers);
    WriteLauncherTicket(value);
    RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 0;
    esp_restart();
}

}  // namespace detail

}  // namespace CardenzaHAL::System
