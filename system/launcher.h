#pragma once

#include <cstdint>

// Back to the 203 Launcher from a firmware, whatever the Launcher's settings say.
//
// The Launcher's bootloader (203-launcher bootloader_components/launcher_boot) starts the Launcher after
// a plain software restart, unless "Resume after reset" is on: then it starts the firmware again. A
// restart ticket in RTC memory overrides that: the bootloader starts the Launcher and the Launcher opens
// its home screen without the "Boot last app" countdown (its own Settings > Restart works this way). The
// ticket is two words in the custom bytes of ESP-IDF's rtc_retain_mem_t, which the Launcher's bootloader
// keeps at the top of RTC fast memory (CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE = 8, not in its CRC):
// the value and its complement, read and cleared once. A bootloader without the Launcher ignores them.
// The constants are the Launcher's (policy.h) and ESP-IDF 5.3's layout; the pure part is tested on the
// host (tests/presence_test.cpp).
namespace CardenzaHAL::System {

    namespace LauncherTicket {
        inline constexpr uint32_t kRestart = 0x32303352u;  // "203R", LAUNCHER_RESTART_TICKET
        // The system menu's Restart (system/menu.h): the next software restart starts the firmware the
        // Launcher started, without its countdown (as the Launcher's own hand-over to a firmware).
        inline constexpr uint32_t kBoot = 0x32303342u;     // "203B", LAUNCHER_BOOT_TICKET
        // The system menu's Turn off: the wake from deep sleep goes through the Launcher, as after a
        // power-on (as the Launcher's own Turn off).
        inline constexpr uint32_t kSleep = 0x32303353u;    // "203S", LAUNCHER_SLEEP_TICKET
        // ESP32-S3: RTC fast memory ends at 0x60100000 (SOC_RTC_DRAM_HIGH); rtc_retain_mem_t with 8 custom
        // bytes is 24 bytes (aligned to 8) below it, its custom bytes 12 bytes into it.
        inline constexpr uint32_t kRtcFastEnd = 0x60100000u;
        inline constexpr uint32_t kRetainMemBytes = 24;
        inline constexpr uint32_t kCustomOffset = 12;
        inline constexpr uint32_t kAddress = kRtcFastEnd - kRetainMemBytes + kCustomOffset;
        inline constexpr uint32_t Inverse(uint32_t value) { return ~value; }
        // As the Launcher's launcher_valid_ticket.
        inline constexpr bool Valid(uint32_t value, uint32_t inverse, uint32_t expected)
        {
            return value == expected && inverse == ~value;
        }
    }  // namespace LauncherTicket

    // Writes the restart ticket, hands the USB PHY back to USB-Serial-JTAG (the Launcher and esptool use
    // it) and restarts. Without the Launcher this restarts the firmware. Shutdown handlers run as for any
    // esp_restart (Wi-Fi stops). Tell a USB host first if it matters: USB::RestartIntoLauncher does that.
    [[noreturn]] void RestartIntoLauncher();

}  // namespace CardenzaHAL::System
