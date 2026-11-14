// Restarting into the 203 Launcher (system/launcher.h).
#include "system/launcher.h"

#include <esp_log.h>
#include <esp_system.h>
#include <soc/rtc_cntl_struct.h>

namespace CardenzaHAL::System {

void RestartIntoLauncher()
{
    ESP_LOGW("CardenzaHAL", "restarting into the Launcher");
    // The top of RTC fast memory is ESP-IDF's reserved segment in every app (its RTC timer data, and the
    // bootloader's data in the Launcher's own build): never heap. The firmware is going away; what it
    // kept there does not matter any more.
    volatile uint32_t* ticket = reinterpret_cast<volatile uint32_t*>(LauncherTicket::kAddress);
    ticket[0] = LauncherTicket::kRestart;
    ticket[1] = LauncherTicket::Inverse(LauncherTicket::kRestart);
    RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 0;
    esp_restart();
}

}  // namespace CardenzaHAL::System
