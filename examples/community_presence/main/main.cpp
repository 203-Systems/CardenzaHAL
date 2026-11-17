// The community presence service in a firmware.
//
// CardenzaHAL::Init starts it (CONFIG_CARDENZA_HAL_PRESENCE): on a board the 203 Launcher bound to the
// community and connected to Wi-Fi, the device is then reachable from the website while this firmware
// runs. Send a firmware to the device from its page: the hook below runs, then the device restarts into
// the Launcher, which installs it (and starts it, for "install and start").
//
// The firmware's part: a hook to save its work before the restart (or refuse); its activity
// (SetActivity: RealTime while it plays sound, Quiet when it wants Wi-Fi's RAM and time back) and the
// internal RAM it keeps for itself (SetRamFloor); Enable(false) when it runs Wi-Fi itself. The log shows
// the service's state and what it reports.
#include <cstdio>

#include <esp_heap_caps.h>
#include <esp_heap_caps_init.h>
#include <esp_log.h>
#include <esp_mmu_map.h>
#include <esp_psram.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include "cardenzaHAL.h"
#include "net/presence.h"

namespace {

#if CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
extern "C" uint8_t _ext_ram_bss_start;
extern "C" uint8_t _ext_ram_bss_end;
#endif

namespace Presence = CardenzaHAL::Presence;
constexpr char kTag[] = "community_presence";

// The PSRAM joins the heap for what asks for it (TLS through Net's allocator), after the .bss ESP-IDF put
// at its start (examples/psram_heap, examples/launcher_net).
void AddPsramToHeap()
{
#if CONFIG_SPIRAM
    if (!esp_psram_is_initialized()) return;
    const size_t size = esp_psram_get_size();
    void* first = nullptr;
    void* last = nullptr;
    if (size == 0 || esp_mmu_paddr_to_vaddr(0, MMU_TARGET_PSRAM0, MMU_VADDR_DATA, &first) != ESP_OK ||
        esp_mmu_paddr_to_vaddr(size - 1, MMU_TARGET_PSRAM0, MMU_VADDR_DATA, &last) != ESP_OK ||
        static_cast<char*>(last) != static_cast<char*>(first) + size - 1)
        return;
    const uint32_t caps[] = {MALLOC_CAP_SPIRAM, 0, MALLOC_CAP_8BIT | MALLOC_CAP_32BIT};
    intptr_t start = reinterpret_cast<intptr_t>(first);
    const intptr_t end = start + static_cast<intptr_t>(size);
#if CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
    const intptr_t bssStart = reinterpret_cast<intptr_t>(&_ext_ram_bss_start);
    const intptr_t bssEnd = reinterpret_cast<intptr_t>(&_ext_ram_bss_end);
    if (bssEnd > bssStart)
    {
        if (bssStart != start || bssEnd >= end) return;
        start = (bssEnd + 15) & ~static_cast<intptr_t>(15);
    }
#endif
    heap_caps_add_region_with_caps(caps, start, end);
#endif
}

// Runs on the service's task before the restart into the Launcher. A firmware saves here what must not
// be lost and may refuse (return false: the website shows "Refused by the running firmware").
bool BeforeHandover(const Presence::Handover& handover, void*)
{
    ESP_LOGW(kTag, "the community sent \"%s\" %s (%s): saving, then restarting into the Launcher",
             handover.firmwareName, handover.version, handover.action);
    return true;
}

const char* StateName(Presence::State state)
{
    switch (state)
    {
    case Presence::State::Off: return "off";
    case Presence::State::Idle: return "idle (not bound, no Wi-Fi saved, or receiving off)";
    case Presence::State::Connecting: return "connecting";
    case Presence::State::Online: return "online";
    case Presence::State::Waiting: return "waiting";
    case Presence::State::SignedOut: return "signed out";
    case Presence::State::HandingOver: return "handing over";
    }
    return "?";
}

}  // namespace

extern "C" void app_main(void)
{
    AddPsramToHeap();
    Presence::SetHandoverHook(BeforeHandover, nullptr);
    // What this firmware keeps of internal RAM for itself; Wi-Fi comes up beside it or not at all.
    Presence::SetRamFloor(32 * 1024);
    CardenzaHAL::Init();  // starts the service
    ESP_LOGI(kTag, "presence planned in this run: %s", Presence::Planned() ? "yes" : "no");
    Presence::Report shown;
    for (;;)
    {
        const Presence::Report report = Presence::GetReport();
        if (report.state != shown.state || report.asked != shown.asked || report.joins != shown.joins)
        {
            shown = report;
            ESP_LOGI(kTag, "presence: %s; asked %u (last %u ms), failed %u, joins %u (Wi-Fi took %u B), internal RAM %u free",
                     StateName(report.state), static_cast<unsigned>(report.asked), static_cast<unsigned>(report.lastAskMs),
                     static_cast<unsigned>(report.failed), static_cast<unsigned>(report.joins),
                     static_cast<unsigned>(report.wifiBytes),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
