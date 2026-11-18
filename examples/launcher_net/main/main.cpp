// Net in a firmware: the 203 Launcher's Wi-Fi and account, one HTTPS request, everything given back.
//
// - ReadLauncherSettings reads what the Launcher saved (its account binding, its Wi-Fi networks and the
//   one it joined last) without writing anything: NVS is opened through a read-only partition entry.
// - Connect joins the network the Launcher joined last, else the strongest saved one in range. Calls
//   block for seconds: they run on a task of their own, below the firmware's UI and audio.
// - Send makes one request over a fresh connection (https checked against ESP-IDF's certificate
//   bundle). Disconnect gives Wi-Fi's memory back.
// The log shows what each step took (time and RAM); it never shows a password or a token.
#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_heap_caps_init.h>
#include <esp_log.h>
#include <esp_mmu_map.h>
#include <esp_psram.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include "net/net.h"

namespace {

#if CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
// Wi-Fi's, lwIP's and the supplicant's .bss, at the start of the PSRAM (sdkconfig.defaults).
extern "C" uint8_t _ext_ram_bss_start;
extern "C" uint8_t _ext_ram_bss_end;
#endif

namespace Net = CardenzaHAL::Net;
constexpr char kTag[] = "launcher_net";
constexpr char kDefaultApi[] = "https://community.203.io/api";

// The PSRAM joins the heap for what asks for it only (examples/psram_heap has the explanation).
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
    // The heap begins after the .bss ESP-IDF put at the start of the PSRAM (examples/psram_heap).
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

void LogHeap(const char* when)
{
    ESP_LOGI(kTag, "%s: internal RAM %u free, PSRAM %u free", when,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

const char* SettingsText(Net::SettingsResult result)
{
    switch (result)
    {
    case Net::SettingsResult::Ok: return "read";
    case Net::SettingsResult::NoPartition: return "no NVS partition (not a Launcher's table)";
    default: return "unreadable without writing (the Launcher repairs it at its next start)";
    }
}

void NetTask(void*)
{
    // About 1.6 KB: kept off the stack.
    static Net::LauncherSettings settings;
    LogHeap("start");
    const Net::SettingsResult read = Net::ReadLauncherSettings(settings);
    ESP_LOGI(kTag, "Launcher settings %s: account %s, %u network(s), last \"%s\" on channel %u", SettingsText(read),
             settings.account.Bound() ? "bound" : "not bound", static_cast<unsigned>(settings.networkCount),
             settings.lastSsid, static_cast<unsigned>(settings.lastChannel));

    Net::ConnectReport report;
    const Net::ConnectStatus status = Net::Connect(settings, report);
    settings.Forget();  // the passwords are not needed any more
    ESP_LOGI(kTag, "Connect: %d (\"%s\", %d dBm, %u ms)", static_cast<int>(status), report.ssid, report.rssi,
             static_cast<unsigned>(report.millis));
    if (status == Net::ConnectStatus::Connected)
    {
        LogHeap("connected");
        char digest[65];
        if (Net::RunningImageDigest(digest)) ESP_LOGI(kTag, "this firmware's image digest: %s", digest);

        // The community's API root answers JSON. A bound device would add its token:
        //   Net::Header{"Authorization", "Bearer <settings.account.token>"} (never log it).
        char url[160];
        std::snprintf(url, sizeof(url), "%s/", settings.account.Bound() ? settings.account.api : kDefaultApi);
        const Net::Header headers[] = {{"Accept", "application/json"}, {"User-Agent", "CardenzaHAL-launcher_net"}};
        Net::Request request;
        request.url = url;
        request.headers = headers;
        request.headerCount = sizeof(headers) / sizeof(headers[0]);
        static char body[1024];
        Net::Response response;
        response.body = body;
        response.capacity = sizeof(body);
        const Net::SendStatus sent = Net::Send(request, response);
        LogHeap("after the request");
        ESP_LOGI(kTag, "GET: %d, HTTP %d, %u bytes%s: %.120s", static_cast<int>(sent), response.status,
                 static_cast<unsigned>(response.length), response.truncated ? " (more)" : "", body);
        Net::Disconnect();
    }
    LogHeap("end");
    ESP_LOGI(kTag, "stack left unused: %u bytes", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    vTaskDelete(nullptr);
}

}  // namespace

extern "C" void app_main(void)
{
    AddPsramToHeap();
    // Net's calls block: a task of their own, low priority, on core 0 beside Wi-Fi.
    xTaskCreatePinnedToCore(NetTask, "net", 8192, nullptr, 2, nullptr, 0);
}
