// Net: the Launcher's settings read without writing, Wi-Fi as a station, HTTP(S) requests.
//
// Reading the Launcher's NVS without ever writing it (ESP-IDF 5.3, components/nvs_flash and
// components/esp_partition):
// - nvs_flash_init_partition_ptr() opens a partition from an esp_partition_t the caller gives. The HAL
//   gives a copy of the "nvs" entry with `readonly` set; NVS keeps the pointer, so the copy is static.
//   Every write NVS can make goes through esp_partition_write / write_raw / erase_range, and those
//   return ESP_ERR_NOT_ALLOWED for a read-only entry before touching the flash (partition_target.c).
//   nvs_open(NVS_READWRITE) on it is refused too (NVSPartitionManager::open_handle).
// - Plain nvs_flash_init() writes at start-up in these states (Storage::init, PageManager::load,
//   Page::load / mLoadEntryTable): a blank partition (it activates a first page); a FREEING page (a
//   page move interrupted: it finishes it, erasing a page); an entry written past the active page's
//   free mark, an ILLEGAL entry or an entry whose header does not match (a write cut by a power loss:
//   it marks them erased); a variable-length item not fully written, an item found twice (power lost
//   between writing the new value and erasing the old), blob indexes and data that do not match. On the
//   read-only copy the refused write makes the open fail (ESP_ERR_NOT_ALLOWED) in the first three; it
//   also fails for a newer NVS format (ESP_ERR_NVS_NEW_VERSION_FOUND) and a partition without a free
//   page (ESP_ERR_NVS_NO_FREE_PAGES), states plain init refuses as well. In the others the error is
//   ignored inside NVS: the item is only dropped in RAM, or, for a fixed-size item found twice, the
//   whole page it is on is marked invalid in RAM and its items vanish while the open succeeds. Each of
//   those changes the count of written entries NVS keeps, so after opening, nvs_get_stats' used_entries
//   is compared with the written entries of the pages as they are on the flash (net/internal/nvs_page.h);
//   any difference, or a page NVS could not count, is Unreadable. The HAL never repairs: the Launcher
//   does that the next time it starts (it opens the partition read-write).
// - When the firmware has already opened "nvs" itself (nvs_get_stats finds it), the HAL reads through
//   that and does not close it: nvs_flash_init_partition_ptr on an open partition would run
//   Storage::init again on the existing, writable storage (NVSPartitionManager::init_custom), and
//   closing it would invalidate the firmware's handles.
// - ESP-IDF 5.3 does not free the small NVSPartition object nvs_flash_init_partition_ptr makes when the
//   partition is closed again (it is not in the list deinit_partition looks in): 8 bytes per read.
//
// - One write, for the presence service's handover (WritePending): the command it took goes into the
//   Launcher's namespace l_pending, right before the restart into the Launcher. NVS is then opened
//   read-write, as the Launcher opens it at its start (which repairs what a cut write left, as it would
//   anyway a moment later); a partition that would have to be erased is left alone, and the handover fails.
//
// Nothing else here touches NVS: Wi-Fi starts with nvs_enable = 0 and WIFI_STORAGE_RAM. The PHY keeps
// its calibration in NVS namespace "phy" when CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE is on (it
// reads it, and writes it after a full calibration); with NVS not open it logs an error and calibrates
// fully every time, with NVS open read-write it would write the Launcher's partition. Firmware using
// Net sets it off (README.md).
#include "net/net.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unistd.h>

#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <esp_tls.h>
#include <esp_attr.h>
#include <esp_private/wifi_os_adapter.h>
#include <esp_wifi.h>
#include <esp_wifi_default.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <sdkconfig.h>
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include <esp_crt_bundle.h>
#endif
#if CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
#include <mbedtls/platform.h>
#endif

#include "net/connect_order.h"
#include "net/internal/nvs_page.h"
#include "net/internal/shared_use.h"
#include "net/presence_logic.h"

#if CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
extern "C" void* esp_mbedtls_mem_calloc(size_t n, size_t size);
extern "C" void esp_mbedtls_mem_free(void* p);
#endif

namespace CardenzaHAL::Net {

namespace {

constexpr char kTag[] = "CardenzaNet";
constexpr char kNvsLabel[] = "nvs";

uint32_t MillisSince(int64_t startUs)
{
    return static_cast<uint32_t>((esp_timer_get_time() - startUs) / 1000);
}

// The presence service connects again and again (after its firmware let it go, after Wi-Fi dropped): its
// routine lines are debug, so that a firmware's log holds news only. Failures stay warnings.
#define NET_LOG_ROUTINE(quiet, ...)                    \
    do                                                 \
    {                                                  \
        if (quiet)                                     \
            ESP_LOGD(kTag, __VA_ARGS__);               \
        else                                           \
            ESP_LOGI(kTag, __VA_ARGS__);               \
    } while (0)

void LogHeap(const char* when, bool quiet = false)
{
    NET_LOG_ROUTINE(quiet, "heap %s: internal %u free (largest %u), PSRAM %u free", when,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

void Wipe(void* data, std::size_t size)
{
    volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) bytes[i] = 0;
}

// Plain data, PSRAM when the heap has some (never touched by DMA or an interrupt).
void* SlowAlloc(std::size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// ---------------------------------------------------------------------------------------------------
// The Launcher's settings

// NVS keeps a pointer to the partition entry it is opened with: the read-only copy lives here.
esp_partition_t g_readOnlyNvs;

// A string into `out`, or empty when it is missing or does not fit (never cut short).
void ReadString(nvs_handle_t handle, const char* key, char* out, std::size_t capacity)
{
    std::size_t length = capacity;
    if (nvs_get_str(handle, key, out, &length) != ESP_OK) out[0] = 0;
}

// The written entries of the partition's pages as they are on the flash; false when NVS would have to
// repair something to open it (a page move to finish, a newer format) or the flash cannot be read.
bool CountWrittenEntries(const esp_partition_t* partition, uint32_t& written)
{
    written = 0;
    uint8_t header[NvsPage::kHeaderBytes + NvsPage::kEntryTableBytes];
    for (std::size_t offset = 0; offset + NvsPage::kPageBytes <= partition->size; offset += NvsPage::kPageBytes)
    {
        if (esp_partition_read_raw(partition, offset, header, sizeof(header)) != ESP_OK) return false;
        const NvsPage::Summary page = NvsPage::Summarize(header, header + NvsPage::kEntryTableOffset);
        if (page.kind == NvsPage::Kind::Freeing || page.kind == NvsPage::Kind::Newer) return false;
        written += page.written;
    }
    return true;
}

// As the Launcher's loadAccount: a partial or foreign record is no binding.
void ReadAccount(LauncherAccount& account)
{
    nvs_handle_t handle = 0;
    if (nvs_open_from_partition(kNvsLabel, "l_account", NVS_READONLY, &handle) != ESP_OK) return;
    ReadString(handle, "token", account.token, sizeof(account.token));
    ReadString(handle, "api", account.api, sizeof(account.api));
    ReadString(handle, "user_id", account.userId, sizeof(account.userId));
    char name[66];  // the Launcher keeps up to 64 bytes
    ReadString(handle, "user_name", name, sizeof(name));
    CopyUtf8(name, account.userName, sizeof(account.userName));
    ReadString(handle, "device_id", account.deviceId, sizeof(account.deviceId));
    nvs_close(handle);
    if (!ValidBinding(account))
    {
        Wipe(account.token, sizeof(account.token));
        account = LauncherAccount{};
    }
}

// As the Launcher's getWifiFromNVS: every s_<suffix> string, its p_<suffix> password and b_<suffix> = 1;
// an entry that is missing a part or does not decrypt is skipped.
void ReadNetworks(LauncherSettings& settings, std::size_t& skipped)
{
    skipped = 0;
    nvs_handle_t handle = 0;
    if (nvs_open_from_partition(kNvsLabel, "l_wifi", NVS_READONLY, &handle) != ESP_OK) return;
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find(kNvsLabel, "l_wifi", NVS_TYPE_STR, &it);
    while (err == ESP_OK && it != nullptr)
    {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (info.key[0] == 's' && info.key[1] == '_')
        {
            WifiNetwork network;
            char key[NVS_KEY_NAME_MAX_SIZE];
            char cipher[90];  // the Launcher writes at most 88 characters
            uint8_t secure = 0;
            ReadString(handle, info.key, network.ssid, sizeof(network.ssid));
            std::memcpy(key, info.key, sizeof(key));  // the same suffix after p_ and b_
            key[0] = 'p';
            std::size_t length = sizeof(cipher);
            bool ok = network.ssid[0] != 0 && nvs_get_str(handle, key, cipher, &length) == ESP_OK;
            key[0] = 'b';
            ok = ok && nvs_get_u8(handle, key, &secure) == ESP_OK && secure == 1 &&
                 LauncherWifi::DecryptPassword(cipher, network.password, sizeof(network.password));
            if (ok && settings.networkCount < kMaxSavedNetworks)
                settings.networks[settings.networkCount++] = network;
            else
                ++skipped;
            Wipe(cipher, sizeof(cipher));
            Wipe(network.password, sizeof(network.password));
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(handle);
}

void ReadLastNetwork(LauncherSettings& settings)
{
    nvs_handle_t handle = 0;
    if (nvs_open_from_partition(kNvsLabel, "launcher", NVS_READONLY, &handle) != ESP_OK) return;
    ReadString(handle, "wifi_last", settings.lastSsid, sizeof(settings.lastSsid));
    if (nvs_get_u8(handle, "wifi_ch", &settings.lastChannel) != ESP_OK) settings.lastChannel = 0;
    nvs_close(handle);
}

// Settings > Account > Receive firmware (launcher/remote_install, u8, missing: on).
bool ReadReceiveFirmware()
{
    nvs_handle_t handle = 0;
    if (nvs_open_from_partition(kNvsLabel, "launcher", NVS_READONLY, &handle) != ESP_OK) return true;
    uint8_t value = 1;
    if (nvs_get_u8(handle, "remote_install", &value) != ESP_OK) value = 1;
    nvs_close(handle);
    return value != 0;
}

// The firmware and the presence service read (and the handover writes) the shared NVS one at a time:
// opening and closing the partition is not safe from two tasks at once.
std::mutex g_settingsMutex;

// Opens "nvs" for reading through the read-only copy of its entry, unless the firmware has it open
// already. `opened`: the HAL opened it (and closes it with CloseAfterReading).
SettingsResult OpenForReading(bool& opened)
{
    opened = false;
    const esp_partition_t* partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, kNvsLabel);
    if (partition == nullptr)
    {
        ESP_LOGW(kTag, "Launcher settings: no NVS partition \"%s\"", kNvsLabel);
        return SettingsResult::NoPartition;
    }
    if (partition->encrypted)
    {
        ESP_LOGW(kTag, "Launcher settings: the NVS partition is encrypted");
        return SettingsResult::Unreadable;
    }
    nvs_stats_t stats = {};
    if (nvs_get_stats(kNvsLabel, &stats) != ESP_ERR_NVS_NOT_INITIALIZED) return SettingsResult::Ok;  // the firmware's
    uint32_t written = 0;
    if (!CountWrittenEntries(partition, written))
    {
        ESP_LOGW(kTag, "Launcher settings: NVS needs a repair or is newer (left to the Launcher)");
        return SettingsResult::Unreadable;
    }
    g_readOnlyNvs = *partition;
    g_readOnlyNvs.readonly = true;
    const esp_err_t err = nvs_flash_init_partition_ptr(&g_readOnlyNvs);
    if (err != ESP_OK)
    {
        ESP_LOGW(kTag, "Launcher settings: NVS cannot be read without writing (%s)", esp_err_to_name(err));
        return SettingsResult::Unreadable;
    }
    const esp_err_t statsErr = nvs_get_stats(kNvsLabel, &stats);
    if (statsErr != ESP_OK || stats.used_entries != written)
    {
        ESP_LOGW(kTag, "Launcher settings: NVS needs a repair (%s, %u of %u entries)", esp_err_to_name(statsErr),
                 static_cast<unsigned>(stats.used_entries), static_cast<unsigned>(written));
        nvs_flash_deinit_partition(kNvsLabel);
        return SettingsResult::Unreadable;
    }
    opened = true;
    return SettingsResult::Ok;
}

void CloseAfterReading(bool opened)
{
    if (opened) nvs_flash_deinit_partition(kNvsLabel);
}

// ---------------------------------------------------------------------------------------------------
// Wi-Fi

constexpr EventBits_t kStarted = BIT0;
constexpr EventBits_t kStopped = BIT1;
constexpr EventBits_t kGotIp = BIT2;
constexpr EventBits_t kFailed = BIT3;
constexpr EventBits_t kScanDone = BIT4;

constexpr int kRetries = 2;               // reconnects within one try (a handshake can time out once)
constexpr uint32_t kAttemptMaxMs = 15000; // one network may not take the whole timeout
constexpr uint16_t kMaxScanRecords = 32;
constexpr TickType_t kPoll = pdMS_TO_TICKS(100);

struct Event {
    esp_event_base_t base;
    int32_t id;
};
// Registered per event (not ESP_EVENT_ANY_ID) so that they run after ESP-IDF's default Wi-Fi handlers
// for the same event: when the HAL sees STA_STOP, the netif has been told, and can be destroyed.
const Event kEvents[] = {
    {WIFI_EVENT, WIFI_EVENT_STA_START},        {WIFI_EVENT, WIFI_EVENT_STA_STOP},
    {WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED}, {WIFI_EVENT, WIFI_EVENT_SCAN_DONE},
    {IP_EVENT, IP_EVENT_STA_GOT_IP},           {IP_EVENT, IP_EVENT_STA_LOST_IP},
};
constexpr std::size_t kEventCount = sizeof(kEvents) / sizeof(kEvents[0]);

struct WifiState {
    EventGroupHandle_t events = nullptr;
    esp_netif_t* netif = nullptr;
    bool ownLoop = false;
    bool wifiInit = false;
    bool started = false;
    esp_event_handler_instance_t handlers[kEventCount] = {};
    // Shared with the event handler (the event loop's task).
    std::atomic<bool> attempting{false};
    std::atomic<int> retries{0};
    std::atomic<int> reason{0};
    std::atomic<int> scanStatus{0};
    char attemptSsid[33] = {};  // written before `attempting` is set
};
WifiState g_wifi;

bool Absent(int reason)
{
    return reason == WIFI_REASON_NO_AP_FOUND || reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
}

// A wrong password: WPA2 times out in the 4-way handshake, WPA3 fails the authentication. A handshake
// can also time out on a weak signal: each try reconnects kRetries times before it counts.
bool PasswordRefused(int reason)
{
    return reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_802_1X_AUTH_FAILED ||
           reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

void OnEvent(void*, esp_event_base_t base, int32_t id, void* data)
{
    WifiState& s = g_wifi;
    if (s.events == nullptr) return;
    if (base == WIFI_EVENT)
    {
        switch (id)
        {
        case WIFI_EVENT_STA_START:
            xEventGroupSetBits(s.events, kStarted);
            break;
        case WIFI_EVENT_STA_STOP:
            xEventGroupClearBits(s.events, kStarted | kGotIp);
            xEventGroupSetBits(s.events, kStopped);
            break;
        case WIFI_EVENT_SCAN_DONE:
            s.scanStatus = data != nullptr ? static_cast<int>(static_cast<wifi_event_sta_scan_done_t*>(data)->status) : 1;
            xEventGroupSetBits(s.events, kScanDone);
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const auto* event = static_cast<const wifi_event_sta_disconnected_t*>(data);
            const int reason = event != nullptr ? static_cast<int>(event->reason) : static_cast<int>(WIFI_REASON_UNSPECIFIED);
            xEventGroupClearBits(s.events, kGotIp);
            if (!s.attempting) break;
            // A late disconnect of the network tried before belongs to that try.
            if (event != nullptr && (event->ssid_len != std::strlen(s.attemptSsid) ||
                                     std::memcmp(event->ssid, s.attemptSsid, event->ssid_len) != 0))
                break;
            s.reason = reason;
            if (!Absent(reason) && reason != WIFI_REASON_AUTH_FAIL && s.retries < kRetries)
            {
                ++s.retries;
                if (esp_wifi_connect() == ESP_OK) break;
            }
            s.attempting = false;
            xEventGroupSetBits(s.events, kFailed);
            break;
        }
        default:
            break;
        }
    }
    else if (base == IP_EVENT)
    {
        if (id == IP_EVENT_STA_GOT_IP)
        {
            s.attempting = false;
            xEventGroupSetBits(s.events, kGotIp);
        }
        else if (id == IP_EVENT_STA_LOST_IP)
        {
            xEventGroupClearBits(s.events, kGotIp);
        }
    }
}

// Everything Connect brought up, taken down in reverse; safe on a partial bring-up.
void TearDown()
{
    WifiState& s = g_wifi;
    s.attempting = false;
    if (s.started)
    {
        esp_wifi_disconnect();
        if (esp_wifi_stop() == ESP_OK && s.events != nullptr)
            xEventGroupWaitBits(s.events, kStopped, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
        s.started = false;
    }
    for (std::size_t i = 0; i < kEventCount; ++i)
    {
        if (s.handlers[i] == nullptr) continue;
        esp_event_handler_instance_unregister(kEvents[i].base, kEvents[i].id, s.handlers[i]);
        s.handlers[i] = nullptr;
    }
    if (s.wifiInit)
    {
        esp_wifi_deinit();
        s.wifiInit = false;
    }
    if (s.netif != nullptr)
    {
        // Unregisters ESP-IDF's default Wi-Fi handlers and detaches the netif from Wi-Fi.
        esp_netif_destroy_default_wifi(s.netif);
        s.netif = nullptr;
    }
    if (s.ownLoop)
    {
        esp_event_loop_delete_default();
        s.ownLoop = false;
    }
    if (s.events != nullptr)
    {
        vEventGroupDelete(s.events);
        s.events = nullptr;
    }
}

// Wi-Fi's own allocations that may live in PSRAM. The driver asks for internal RAM where it needs it (its
// DMA receive buffers, queues, task: _malloc_internal and the task wrappers) and for the rest through
// _wifi_malloc / _wifi_calloc / _wifi_zalloc / _wifi_realloc: its configuration (3.5 KB), the station, the
// management frames (5 x 600 B) and the frames it copies for lwIP while they come in. ESP-IDF's
// CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP sends exactly these to PSRAM first (esp_adapter.c's wifi_malloc), but
// only with SPIRAM_USE_CAPS_ALLOC or SPIRAM_USE_MALLOC; a firmware that adds its PSRAM to the heap itself for
// MALLOC_CAP_SPIRAM alone (CONFIG_SPIRAM_USE_MEMMAP, examples/psram_heap) gets the same here, from Net's own
// copy of the adapter table. lwIP stays as ESP-IDF builds it (internal RAM). Without PSRAM in the heap,
// PSRAM full, or with ESP-IDF's option on, the driver's own table is used and nothing changes. Measured on a
// Cardenza (CardenzaSynth, ESP-IDF 5.3.5): Wi-Fi up took 38.6 KB of internal RAM with the driver's table.
IRAM_ATTR void* WifiSlowMalloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

IRAM_ATTR void* WifiSlowRealloc(void* ptr, size_t size)
{
    return heap_caps_realloc_prefer(ptr, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

IRAM_ATTR void* WifiSlowCalloc(size_t n, size_t size)
{
    return heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

IRAM_ATTR void* WifiSlowZalloc(size_t size)
{
    return WifiSlowCalloc(1, size);
}

// The adapter table esp_wifi_init gets: the driver's own, or a copy with the allocations above. It has to
// outlive esp_wifi_deinit (the driver keeps the pointer), so it is static; written only by BringUp.
DRAM_ATTR wifi_osi_funcs_t g_wifiFuncs;
std::atomic<bool> g_wifiInPsram{false};  // the running Wi-Fi was started with the copy

// Some PSRAM in the heap, with room beyond what Wi-Fi takes (about 20 KB while frames come and go).
bool WifiPsramUsable()
{
#if CONFIG_SPIRAM && !CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= 64 * 1024;
#else
    return false;
#endif
}

wifi_osi_funcs_t* WifiFuncs()
{
#if CONFIG_SPIRAM && !CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP
    if (WifiPsramUsable())
    {
        g_wifiFuncs = g_wifi_osi_funcs;
        g_wifiFuncs._wifi_malloc = WifiSlowMalloc;
        g_wifiFuncs._wifi_realloc = WifiSlowRealloc;
        g_wifiFuncs._wifi_calloc = WifiSlowCalloc;
        g_wifiFuncs._wifi_zalloc = WifiSlowZalloc;
        g_wifiInPsram = true;
        return &g_wifiFuncs;
    }
#endif
    g_wifiInPsram = false;
    return &g_wifi_osi_funcs;
}

ConnectStatus StatusFor(esp_err_t err)
{
    return err == ESP_ERR_NO_MEM ? ConnectStatus::NoMemory : ConnectStatus::Failed;
}

ConnectStatus BringUp(esp_err_t& error)
{
    WifiState& s = g_wifi;
    error = ESP_OK;
    // Wi-Fi is the firmware's or the HAL's, not both: one that is running already is left alone.
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_ERR_WIFI_NOT_INIT || esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") != nullptr)
    {
        ESP_LOGE(kTag, "Wi-Fi is already in use outside Net");
        error = ESP_ERR_INVALID_STATE;
        return ConnectStatus::Failed;
    }
    s.events = xEventGroupCreate();
    if (s.events == nullptr)
    {
        error = ESP_ERR_NO_MEM;
        return ConnectStatus::NoMemory;
    }
    // lwIP's task: started once, never stopped (esp_netif_deinit is not supported in ESP-IDF 5.3).
    if ((error = esp_netif_init()) != ESP_OK) return StatusFor(error);
    error = esp_event_loop_create_default();
    if (error == ESP_OK)
        s.ownLoop = true;
    else if (error != ESP_ERR_INVALID_STATE)  // the firmware's loop: used, and left to it
        return StatusFor(error);
    error = ESP_OK;

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    config.nvs_enable = 0;
    config.osi_funcs = WifiFuncs();
    if ((error = esp_wifi_init(&config)) != ESP_OK) return StatusFor(error);
    s.wifiInit = true;
    if ((error = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return StatusFor(error);

    // esp_netif_create_default_wifi_sta() aborts when it runs out of memory: the same steps, checked.
    const esp_netif_config_t netifConfig = ESP_NETIF_DEFAULT_WIFI_STA();
    s.netif = esp_netif_new(&netifConfig);
    if (s.netif == nullptr)
    {
        error = ESP_ERR_NO_MEM;
        return ConnectStatus::NoMemory;
    }
    if ((error = esp_netif_attach_wifi_station(s.netif)) != ESP_OK) return StatusFor(error);
    if ((error = esp_wifi_set_default_wifi_sta_handlers()) != ESP_OK) return StatusFor(error);
    for (std::size_t i = 0; i < kEventCount; ++i)
        if ((error = esp_event_handler_instance_register(kEvents[i].base, kEvents[i].id, OnEvent, nullptr,
                                                         &s.handlers[i])) != ESP_OK)
            return StatusFor(error);

    if ((error = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return StatusFor(error);
    if ((error = esp_wifi_start()) != ESP_OK) return StatusFor(error);
    s.started = true;
    // Full power while it is up: power save slows uploads down, and Net is up only while it is used.
    esp_wifi_set_ps(WIFI_PS_NONE);
    if ((xEventGroupWaitBits(s.events, kStarted, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000)) & kStarted) == 0)
    {
        error = ESP_ERR_TIMEOUT;
        return ConnectStatus::Failed;
    }
    return ConnectStatus::Connected;
}

enum class Outcome : uint8_t { Connected, Failed, Cancelled, TimedOut, Error };

Outcome Try(const WifiNetwork& network, uint8_t channel, int64_t deadlineUs, CancelCheck cancelled, void* context,
            esp_err_t& error)
{
    WifiState& s = g_wifi;
    // Leave the network tried before, and let its disconnect pass.
    s.attempting = false;
    esp_wifi_disconnect();
    vTaskDelay(kPoll);
    xEventGroupClearBits(s.events, kGotIp | kFailed);

    wifi_config_t config = {};
    const std::size_t ssidLength = strnlen(network.ssid, sizeof(config.sta.ssid));
    std::memcpy(config.sta.ssid, network.ssid, ssidLength);
    std::memcpy(config.sta.password, network.password, strnlen(network.password, sizeof(network.password) - 1));
    // As the Launcher: any security; with a password of 8 characters or more ESP-IDF then still wants
    // WPA2 at least.
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    // On a known channel the driver looks there first and stops at the first match; without one it
    // scans every channel.
    config.sta.channel = channel;
    config.sta.scan_method = channel != 0 ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    error = esp_wifi_set_config(WIFI_IF_STA, &config);
    Wipe(&config, sizeof(config));
    if (error != ESP_OK) return Outcome::Error;

    std::memcpy(s.attemptSsid, network.ssid, ssidLength);
    s.attemptSsid[ssidLength] = 0;
    s.retries = 0;
    s.reason = 0;
    s.attempting = true;
    if ((error = esp_wifi_connect()) != ESP_OK)
    {
        s.attempting = false;
        return Outcome::Error;
    }
    const int64_t attemptEndUs = std::min<int64_t>(deadlineUs, esp_timer_get_time() + int64_t{kAttemptMaxMs} * 1000);
    while (true)
    {
        const EventBits_t bits = xEventGroupWaitBits(s.events, kGotIp | kFailed, pdFALSE, pdFALSE, kPoll);
        if ((bits & kGotIp) != 0) return Outcome::Connected;
        if ((bits & kFailed) != 0) return Outcome::Failed;
        const bool stop = cancelled != nullptr && cancelled(context);
        if (stop || esp_timer_get_time() >= attemptEndUs)
        {
            s.attempting = false;
            esp_wifi_disconnect();
            return stop ? Outcome::Cancelled : Outcome::TimedOut;
        }
    }
}

// Scans (active, 50-120 ms a channel: about 1.5 s for 13 channels) and gives the networks found, in
// a block from SlowAlloc the caller frees. -1: failed, -2: cancelled, -3: no memory.
int Scan(ConnectOrder::Seen*& seen, CancelCheck cancelled, void* context)
{
    WifiState& s = g_wifi;
    seen = nullptr;
    wifi_scan_config_t config = {};
    config.show_hidden = false;  // a hidden network has no SSID to match
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    config.scan_time.active.min = 50;
    config.scan_time.active.max = 120;
    xEventGroupClearBits(s.events, kScanDone);
    if (esp_wifi_scan_start(&config, false) != ESP_OK) return -1;
    const int64_t endUs = esp_timer_get_time() + 15 * 1000 * 1000;
    while ((xEventGroupWaitBits(s.events, kScanDone, pdFALSE, pdFALSE, kPoll) & kScanDone) == 0)
    {
        const bool stop = cancelled != nullptr && cancelled(context);
        if (stop || esp_timer_get_time() >= endUs)
        {
            esp_wifi_scan_stop();
            esp_wifi_clear_ap_list();
            return stop ? -2 : -1;
        }
    }
    if (s.scanStatus != 0)
    {
        esp_wifi_clear_ap_list();
        return -1;
    }
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count == 0)
    {
        esp_wifi_clear_ap_list();
        return 0;
    }
    count = std::min(count, kMaxScanRecords);
    auto* records = static_cast<wifi_ap_record_t*>(SlowAlloc(sizeof(wifi_ap_record_t) * count));
    seen = static_cast<ConnectOrder::Seen*>(SlowAlloc(sizeof(ConnectOrder::Seen) * count));
    if (records == nullptr || seen == nullptr)
    {
        heap_caps_free(records);
        heap_caps_free(seen);
        seen = nullptr;
        esp_wifi_clear_ap_list();
        return -3;
    }
    // Takes the strongest `count` and frees ESP-IDF's list.
    if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK)
    {
        heap_caps_free(records);
        heap_caps_free(seen);
        seen = nullptr;
        esp_wifi_clear_ap_list();
        return -1;
    }
    for (uint16_t i = 0; i < count; ++i)
    {
        seen[i] = ConnectOrder::Seen{};
        std::memcpy(seen[i].ssid, records[i].ssid, strnlen(reinterpret_cast<const char*>(records[i].ssid), 32));
        seen[i].rssi = records[i].rssi;
        seen[i].channel = records[i].primary;
    }
    heap_caps_free(records);
    return count;
}

// ---------------------------------------------------------------------------------------------------
// TLS memory

#if CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
std::atomic<bool> g_tlsAllocatorSet{false};
#endif

// With CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC, ESP-IDF 5.3 leaves mbedTLS on plain calloc / free
// (mbedtls/port/include/mbedtls/esp_config.h only maps them to esp_mbedtls_mem_calloc in the other
// modes): the allocator below is installed with mbedtls_platform_set_calloc_free before the first TLS
// connection. Blocks from plain calloc and from it are freed alike (both are the ESP-IDF heap).
void UseTlsAllocator()
{
#if CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
    if (!g_tlsAllocatorSet.exchange(true)) mbedtls_platform_set_calloc_free(esp_mbedtls_mem_calloc, esp_mbedtls_mem_free);
#endif
}

// ---------------------------------------------------------------------------------------------------
// HTTP

struct SendContext {
    esp_err_t tlsError = ESP_OK;
    int tlsCode = 0;   // mbedTLS's error, as a positive number
    int certFlags = 0; // mbedTLS's certificate verification flags
};

esp_err_t OnHttpEvent(esp_http_client_event_t* event)
{
    if (event->event_id == HTTP_EVENT_ERROR && event->data != nullptr && event->user_data != nullptr)
    {
        auto* context = static_cast<SendContext*>(event->user_data);
        int code = 0;
        int flags = 0;
        const esp_err_t last =
            esp_tls_get_and_clear_last_error(static_cast<esp_tls_error_handle_t>(event->data), &code, &flags);
        if (last != ESP_OK) context->tlsError = last;
        if (code != 0) context->tlsCode = code;
        if (flags != 0) context->certFlags = flags;
    }
    return ESP_OK;
}

bool TlsOutOfMemory(const SendContext& context)
{
    switch (context.tlsCode)
    {
    case 0x7F00:  // MBEDTLS_ERR_SSL_ALLOC_FAILED
    case 0x2880:  // MBEDTLS_ERR_X509_ALLOC_FAILED
    case 0x0010:  // MBEDTLS_ERR_MPI_ALLOC_FAILED
    case 0x3F80:  // MBEDTLS_ERR_PK_ALLOC_FAILED
    case 0x4D80:  // MBEDTLS_ERR_ECP_ALLOC_FAILED
    case 0x6180:  // MBEDTLS_ERR_CIPHER_ALLOC_FAILED
    case 0x5180:  // MBEDTLS_ERR_MD_ALLOC_FAILED
        return true;
    default:
        return context.tlsError == ESP_ERR_NO_MEM;
    }
}

esp_http_client_method_t MethodOf(const char* method)
{
    if (std::strcmp(method, "POST") == 0) return HTTP_METHOD_POST;
    if (std::strcmp(method, "PUT") == 0) return HTTP_METHOD_PUT;
    if (std::strcmp(method, "DELETE") == 0) return HTTP_METHOD_DELETE;
    if (std::strcmp(method, "HEAD") == 0) return HTTP_METHOD_HEAD;
    if (std::strcmp(method, "PATCH") == 0) return HTTP_METHOD_PATCH;
    return HTTP_METHOD_GET;
}

// All of `data`, or false.
bool WriteAll(esp_http_client_handle_t client, const char* data, std::size_t length)
{
    while (length > 0)
    {
        const int written = esp_http_client_write(client, data, static_cast<int>(length));
        if (written <= 0) return false;
        data += written;
        length -= static_cast<std::size_t>(written);
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------

static SettingsResult ReadSettingsNow(LauncherSettings& settings, bool quiet)
{
    // Cleared in place: a whole LauncherSettings{} would be a 1.6 KB temporary on the caller's stack.
    settings.Forget();
    settings.account = LauncherAccount{};
    for (WifiNetwork& network : settings.networks) std::memset(network.ssid, 0, sizeof(network.ssid));
    settings.networkCount = 0;
    std::memset(settings.lastSsid, 0, sizeof(settings.lastSsid));
    settings.lastChannel = 0;
    settings.receiveFirmware = true;
    const int64_t startUs = esp_timer_get_time();
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    bool opened = false;
    const SettingsResult result = OpenForReading(opened);
    if (result != SettingsResult::Ok) return result;

    std::size_t skipped = 0;
    ReadAccount(settings.account);
    ReadNetworks(settings, skipped);
    ReadLastNetwork(settings);
    settings.receiveFirmware = ReadReceiveFirmware();
    CloseAfterReading(opened);

    ConnectOrder::Attempt last;
    NET_LOG_ROUTINE(quiet, "Launcher settings read in %u ms: account %s, %u saved network(s)%s, last network %s",
             static_cast<unsigned>(MillisSince(startUs)), settings.account.Bound() ? "bound" : "not bound",
             static_cast<unsigned>(settings.networkCount), skipped != 0 ? " (some unreadable, skipped)" : "",
             ConnectOrder::LastNetwork(settings, last) ? "known" : "unknown");
    if (skipped != 0) ESP_LOGW(kTag, "Launcher settings: %u Wi-Fi entr%s skipped", static_cast<unsigned>(skipped),
                               skipped == 1 ? "y" : "ies");
    return SettingsResult::Ok;
}

SettingsResult ReadLauncherSettings(LauncherSettings& settings)
{
    return ReadSettingsNow(settings, false);
}

SettingsResult Internal::ReadSettings(LauncherSettings& settings, bool quiet)
{
    return ReadSettingsNow(settings, quiet);
}

// Connect, one caller at a time (g_connectMutex).
static ConnectStatus ConnectNow(const LauncherSettings& settings, ConnectReport& report, uint32_t timeoutMs,
                                CancelCheck cancelled, void* context, bool quiet)
{
    report = ConnectReport{};
    const int64_t startUs = esp_timer_get_time();
    if (Connected())
    {
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        {
            std::memcpy(report.ssid, ap.ssid, strnlen(reinterpret_cast<const char*>(ap.ssid), 32));
            report.rssi = ap.rssi;
        }
        return ConnectStatus::Connected;
    }
    if (g_wifi.events != nullptr) TearDown();  // up but without an address: start over
    const std::size_t networkCount = std::min(settings.networkCount, kMaxSavedNetworks);
    if (networkCount == 0)
    {
        ESP_LOGW(kTag, "Connect: the Launcher has no saved network");
        return ConnectStatus::NoNetworks;
    }

    LogHeap("before Connect", quiet);
    UseTlsAllocator();
    ConnectStatus status = BringUp(report.error);
    if (status != ConnectStatus::Connected)
    {
        ESP_LOGE(kTag, "Connect: Wi-Fi did not start (%s)", esp_err_to_name(report.error));
        TearDown();
        report.millis = MillisSince(startUs);
        return status;
    }

    const int64_t deadlineUs = startUs + int64_t{timeoutMs} * 1000;
    bool connected = false;
    bool anyTried = false;
    bool passwordRefused = false;
    bool otherFailure = false;
    status = ConnectStatus::NotFound;
    auto attempt = [&](const ConnectOrder::Attempt& a) {
        const WifiNetwork& network = settings.networks[a.network];
        std::snprintf(report.ssid, sizeof(report.ssid), "%s", network.ssid);
        const int64_t tryStartUs = esp_timer_get_time();
        NET_LOG_ROUTINE(quiet, "Connect: trying \"%s\" (channel %u)", network.ssid, static_cast<unsigned>(a.channel));
        anyTried = true;
        esp_err_t err = ESP_OK;
        const Outcome outcome = Try(network, a.channel, deadlineUs, cancelled, context, err);
        const int reason = g_wifi.reason;
        switch (outcome)
        {
        case Outcome::Connected:
            connected = true;
            break;
        case Outcome::Cancelled:
            status = ConnectStatus::Cancelled;
            break;
        case Outcome::Failed:
            ESP_LOGW(kTag, "Connect: \"%s\" failed after %u ms (reason %d)", network.ssid,
                     static_cast<unsigned>(MillisSince(tryStartUs)), reason);
            if (PasswordRefused(reason))
                passwordRefused = true;
            else if (!Absent(reason))
                otherFailure = true;
            break;
        case Outcome::TimedOut:
            ESP_LOGW(kTag, "Connect: \"%s\" gave no address in %u ms", network.ssid,
                     static_cast<unsigned>(MillisSince(tryStartUs)));
            otherFailure = true;
            break;
        case Outcome::Error:
            ESP_LOGW(kTag, "Connect: \"%s\": %s", network.ssid, esp_err_to_name(err));
            report.error = err;
            otherFailure = true;
            if (err == ESP_ERR_NO_MEM) status = ConnectStatus::NoMemory;
            break;
        }
    };
    auto stopped = [&] {
        return connected || status == ConnectStatus::Cancelled || status == ConnectStatus::NoMemory ||
               esp_timer_get_time() >= deadlineUs;
    };

    ConnectOrder::Attempt last;
    const bool haveLast = ConnectOrder::LastNetwork(settings, last);
    if (haveLast) attempt(last);
    if (!stopped() && ConnectOrder::WorthScanning(settings, haveLast))
    {
        ConnectOrder::Seen* seen = nullptr;
        const int64_t scanStartUs = esp_timer_get_time();
        const int found = Scan(seen, cancelled, context);
        if (found == -2)
            status = ConnectStatus::Cancelled;
        else if (found == -3)
            status = ConnectStatus::NoMemory;
        else if (found < 0)
            otherFailure = true;
        NET_LOG_ROUTINE(quiet, "Connect: scan found %d network(s) in %u ms", found < 0 ? 0 : found,
                 static_cast<unsigned>(MillisSince(scanStartUs)));
        ConnectOrder::Attempt order[kMaxSavedNetworks];
        const std::size_t count = found > 0 ? ConnectOrder::AfterScan(settings, seen, static_cast<std::size_t>(found),
                                                                      haveLast ? last.network : -1, order,
                                                                      kMaxSavedNetworks)
                                            : 0;
        heap_caps_free(seen);
        for (std::size_t i = 0; i < count && !stopped(); ++i) attempt(order[i]);
    }

    report.millis = MillisSince(startUs);
    if (connected)
    {
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) report.rssi = ap.rssi;
        esp_netif_ip_info_t ip = {};
        esp_netif_get_ip_info(g_wifi.netif, &ip);
        report.error = ESP_OK;
        NET_LOG_ROUTINE(quiet, "Connect: joined \"%s\" (channel %u, %d dBm, " IPSTR ") in %u ms", report.ssid,
                 static_cast<unsigned>(ap.primary), ap.rssi, IP2STR(&ip.ip), static_cast<unsigned>(report.millis));
        LogHeap("after Connect", quiet);
        return ConnectStatus::Connected;
    }
    if (status != ConnectStatus::Cancelled && status != ConnectStatus::NoMemory)
    {
        if (passwordRefused)
            status = ConnectStatus::AuthFailed;
        else if (otherFailure || (anyTried && esp_timer_get_time() >= deadlineUs))
            status = ConnectStatus::Failed;
        else
            status = ConnectStatus::NotFound;
    }
    if (report.error == ESP_OK)
        report.error = status == ConnectStatus::Cancelled ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    static const char* const kNames[] = {"connected", "no networks", "not found", "password refused",
                                         "failed",    "no memory",   "cancelled"};
    ESP_LOGW(kTag, "Connect: %s after %u ms", kNames[static_cast<int>(status)], static_cast<unsigned>(report.millis));
    TearDown();
    LogHeap("after a failed Connect");
    return status;
}

bool Connected()
{
    return g_wifi.events != nullptr && (xEventGroupGetBits(g_wifi.events) & kGotIp) != 0;
}

// A client kept from one request to the next (the presence service's): https to one host, its TLS session
// saved after each handshake and offered at the next connection (CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS:
// session ids and tickets), so a request after the first resumes it: no certificate chain on the air, no
// key exchange or certificate check on the CPU. The connection itself is closed after each request (one
// TCP connection per request, nothing held open between them). The handle keeps its two 1 KB buffers
// (internal RAM) and the session (TLS memory) until ForgetKept.
struct KeptClient {
    esp_http_client_handle_t handle = nullptr;
    char host[64] = {};
    uint16_t port = 0;
};

void ForgetKept(KeptClient& kept)
{
    if (kept.handle != nullptr) esp_http_client_cleanup(kept.handle);
    kept = KeptClient{};
}

constexpr int kKeptBufferBytes = 1024;

// quiet: a success is logged at debug level only (the presence service's polls would fill a firmware's log).
// kept: the caller's kept client (https only; nullptr: a client of its own for this request).
static SendStatus SendNow(const Request& request, Response& response, Progress progress, CancelCheck cancelled,
                         void* context, bool quiet, KeptClient* kept)
{
    response.status = 0;
    response.length = 0;
    response.truncated = false;
    response.error = ESP_OK;
    if (response.body != nullptr && response.capacity > 0) response.body[0] = 0;
    UrlParts parts;
    if (!ValidRequest(request) || !ParseUrl(request.url, parts))
    {
        ESP_LOGE(kTag, "Send: the request is not valid");
        response.error = ESP_ERR_INVALID_ARG;
        return SendStatus::InvalidRequest;
    }
    // The host for the log; never the path and query (signed URLs carry credentials in them).
    char host[64];
    std::snprintf(host, sizeof(host), "%.*s", static_cast<int>(std::min<std::size_t>(parts.hostLength, 63)), parts.host);
    if (!Connected())
    {
        response.error = ESP_ERR_INVALID_STATE;
        return SendStatus::NotConnected;
    }
    UseTlsAllocator();
    const int64_t startUs = esp_timer_get_time();

    SendContext tls;
    esp_http_client_config_t config = {};
    config.url = request.url;
    config.method = MethodOf(request.method);
    config.timeout_ms = request.timeoutMs > 0 ? static_cast<int>(std::min<uint32_t>(request.timeoutMs, INT_MAX)) : 20000;
    config.disable_auto_redirect = true;
    config.event_handler = OnHttpEvent;
    config.user_data = &tls;
    config.buffer_size = 1024;
    // The request line (method, path and query) has to fit in the send buffer.
    config.buffer_size_tx = static_cast<int>(std::max<std::size_t>(1024, std::strlen(request.url) + 64));
    for (std::size_t i = 0; i < request.headerCount; ++i)
        config.buffer_size_tx = static_cast<int>(std::max<std::size_t>(
            static_cast<std::size_t>(config.buffer_size_tx),
            std::strlen(request.headers[i].name) + std::strlen(request.headers[i].value) + 8));
    if (parts.https)
    {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        config.crt_bundle_attach = esp_crt_bundle_attach;
#else
        ESP_LOGE(kTag, "Send: https needs CONFIG_MBEDTLS_CERTIFICATE_BUNDLE");
        response.error = ESP_ERR_NOT_SUPPORTED;
        return SendStatus::ConnectFailed;
#endif
    }

#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
    // A kept client serves this request when it is https to the same host and the request line and the
    // headers fit its buffer.
    if (kept != nullptr && (!parts.https || config.buffer_size_tx > kKeptBufferBytes)) kept = nullptr;
#else
    kept = nullptr;
#endif
    bool reused = false;
    if (kept != nullptr)
    {
        if (kept->handle != nullptr && (std::strcmp(kept->host, host) != 0 || kept->port != parts.port)) ForgetKept(*kept);
        reused = kept->handle != nullptr;
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
        config.save_client_session = true;
#endif
        config.buffer_size = kKeptBufferBytes;
        config.buffer_size_tx = kKeptBufferBytes;
    }
    esp_http_client_handle_t client = reused ? kept->handle : esp_http_client_init(&config);
    if (client == nullptr)
    {
        response.error = ESP_ERR_NO_MEM;
        return SendStatus::NoMemory;
    }
    if (kept != nullptr && !reused)
    {
        kept->handle = client;
        std::snprintf(kept->host, sizeof(kept->host), "%s", host);
        kept->port = parts.port;
    }
    if (reused)
    {
        // The request's own URL, method, timeout and context; the header that only some requests have goes.
        esp_err_t err = esp_http_client_set_url(client, request.url);
        if (err == ESP_OK) err = esp_http_client_set_method(client, config.method);
        if (err == ESP_OK) err = esp_http_client_set_timeout_ms(client, config.timeout_ms);
        if (err == ESP_OK) err = esp_http_client_set_user_data(client, &tls);
        (void)esp_http_client_delete_header(client, "Content-Type");
        if (err != ESP_OK)
        {
            ForgetKept(*kept);
            response.error = err;
            return SendStatus::NoMemory;
        }
    }
    SendStatus status = SendStatus::Ok;
    uint8_t* block = nullptr;
    uint64_t sent = 0;
    const bool fileBody = request.bodyFd >= 0;
    const uint64_t total = fileBody ? request.bodyFileLength : request.bodyLength;
    auto stop = [&] { return cancelled != nullptr && cancelled(context); };

    for (std::size_t i = 0; i < request.headerCount && status == SendStatus::Ok; ++i)
        if ((response.error = esp_http_client_set_header(client, request.headers[i].name, request.headers[i].value)) != ESP_OK)
            status = SendStatus::NoMemory;
    if (status == SendStatus::Ok && fileBody)
    {
        // The SD card's DMA cannot reach PSRAM: the block it reads into is internal.
        block = static_cast<uint8_t*>(heap_caps_malloc(kUploadBlockBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
        if (block == nullptr)
        {
            response.error = ESP_ERR_NO_MEM;
            status = SendStatus::NoMemory;
        }
    }
    if (status == SendStatus::Ok && stop()) status = SendStatus::Cancelled;
    if (status == SendStatus::Ok)
    {
        const esp_err_t err = esp_http_client_open(client, static_cast<int>(total));
        if (err != ESP_OK)
        {
            response.error = err;
            status = TlsOutOfMemory(tls) || err == ESP_ERR_NO_MEM ? SendStatus::NoMemory
                     : err == ESP_ERR_HTTP_WRITE_DATA            ? SendStatus::SendFailed
                                                                 : SendStatus::ConnectFailed;
            if (tls.certFlags != 0)
                ESP_LOGE(kTag, "Send: %s's certificate is not trusted (flags 0x%x)", host, static_cast<unsigned>(tls.certFlags));
            else if (tls.tlsCode != 0)
                ESP_LOGE(kTag, "Send: TLS to %s failed (mbedTLS -0x%04x)", host, static_cast<unsigned>(tls.tlsCode));
        }
    }

    // The body, a block at a time, with progress and a chance to cancel after each.
    while (status == SendStatus::Ok && sent < total)
    {
        const std::size_t want = static_cast<std::size_t>(std::min<uint64_t>(total - sent, kUploadBlockBytes));
        const char* data = nullptr;
        std::size_t length = want;
        if (fileBody)
        {
            ssize_t got;
            do got = read(request.bodyFd, block, want);
            while (got < 0 && errno == EINTR);
            if (got <= 0)
            {
                ESP_LOGE(kTag, "Send: reading the body file failed at %llu of %llu bytes (errno %d)",
                         static_cast<unsigned long long>(sent), static_cast<unsigned long long>(total), got < 0 ? errno : 0);
                response.error = ESP_FAIL;
                status = SendStatus::SendFailed;
                break;
            }
            data = reinterpret_cast<const char*>(block);
            length = static_cast<std::size_t>(got);
        }
        else
        {
            data = static_cast<const char*>(request.body) + sent;
        }
        if (!WriteAll(client, data, length))
        {
            response.error = ESP_ERR_HTTP_WRITE_DATA;
            status = SendStatus::SendFailed;
            break;
        }
        sent += length;
        if (progress != nullptr && !progress(context, sent, total)) status = SendStatus::Cancelled;
        else if (stop()) status = SendStatus::Cancelled;
    }
    heap_caps_free(block);

    // The answer.
    if (status == SendStatus::Ok)
    {
        const int64_t headers = esp_http_client_fetch_headers(client);
        if (headers < 0)
        {
            response.error = headers == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_ERR_HTTP_FETCH_HEADER;
            status = SendStatus::ReadFailed;
        }
        else
        {
            response.status = esp_http_client_get_status_code(client);
        }
    }
    const bool head = config.method == HTTP_METHOD_HEAD;
    bool ended = head;  // a HEAD answer has no body
    while (status == SendStatus::Ok && !ended)
    {
        char spare[16];
        const std::size_t room =
            response.body != nullptr && response.capacity > response.length + 1 ? response.capacity - 1 - response.length : 0;
        // With no room left, one more byte says whether the body was longer.
        char* into = room > 0 ? response.body + response.length : spare;
        const int want = static_cast<int>(room > 0 ? std::min<std::size_t>(room, INT_MAX) : 1);
        const int got = esp_http_client_read(client, into, want);
        if (got < 0)
        {
            response.error = got == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
            status = SendStatus::ReadFailed;
        }
        else if (got == 0)
        {
            ended = true;
        }
        else if (room > 0)
        {
            response.length += static_cast<std::size_t>(got);
        }
        else
        {
            response.truncated = true;
            break;
        }
        if (status == SendStatus::Ok && !ended && stop()) status = SendStatus::Cancelled;
    }
    // The connection closed before the whole body came.
    if (status == SendStatus::Ok && ended && !head && !esp_http_client_is_complete_data_received(client))
    {
        response.error = ESP_ERR_HTTP_CONNECTION_CLOSED;
        status = SendStatus::ReadFailed;
    }
    if (response.body != nullptr && response.capacity > 0) response.body[response.length] = 0;
    if (kept == nullptr)
        esp_http_client_cleanup(client);  // closes the connection too
    else if (status == SendStatus::Ok)
        esp_http_client_close(client);  // the next request opens a new connection, resuming the session
    else
        ForgetKept(*kept);  // nothing of a connection that failed is offered again

    static const char* const kNames[] = {"ok", "invalid", "not connected", "connect failed", "send failed",
                                         "read failed", "cancelled", "no memory"};
    if (status == SendStatus::Ok && quiet)
        ESP_LOGD(kTag, "%s %s: %d, sent %llu B, received %u B%s, %u ms%s", request.method, host, response.status,
                 static_cast<unsigned long long>(sent), static_cast<unsigned>(response.length),
                 response.truncated ? " (truncated)" : "", static_cast<unsigned>(MillisSince(startUs)),
                 reused ? ", session offered" : "");
    else if (status == SendStatus::Ok)
        ESP_LOGI(kTag, "%s %s: %d, sent %llu B, received %u B%s, %u ms", request.method, host, response.status,
                 static_cast<unsigned long long>(sent), static_cast<unsigned>(response.length),
                 response.truncated ? " (truncated)" : "", static_cast<unsigned>(MillisSince(startUs)));
    else
        ESP_LOGW(kTag, "%s %s: %s (%s), status %d, sent %llu of %llu B, %u ms", request.method, host,
                 kNames[static_cast<int>(status)], esp_err_to_name(response.error), response.status,
                 static_cast<unsigned long long>(sent), static_cast<unsigned long long>(total),
                 static_cast<unsigned>(MillisSince(startUs)));
    return status;
}

// ---------------------------------------------------------------------------------------------------
// One connection for the firmware and the presence service

namespace {

std::mutex g_connectMutex;
uint8_t g_holders = 0;  // Internal::User bits, under g_connectMutex
std::atomic<int> g_sending[3] = {};
// The presence service's kept client (its task's alone: one request at a time).
KeptClient g_presenceClient;

uint8_t Bit(Internal::User user)
{
    return static_cast<uint8_t>(user);
}

// Modem sleep while only the presence service holds Wi-Fi (it asks every 15 s or so: fewer wake-ups and
// less power); full power while the firmware uses it (uploads).
void ApplyPowerSave()
{
    if (!Connected()) return;
    esp_wifi_set_ps((g_holders & Bit(Internal::User::Firmware)) != 0 ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
}

}  // namespace

ConnectStatus Internal::Connect(User user, const LauncherSettings& settings, ConnectReport& report,
                                uint32_t timeoutMs, CancelCheck cancelled, void* context, bool quiet)
{
    std::lock_guard<std::mutex> lock(g_connectMutex);
    const ConnectStatus status = ConnectNow(settings, report, timeoutMs, cancelled, context, quiet);
    if (status == ConnectStatus::Connected)
        g_holders |= Bit(user);
    else
        g_holders &= static_cast<uint8_t>(~Bit(user));
    ApplyPowerSave();
    return status;
}

void Internal::Disconnect(User user)
{
    // The presence service lets go when its firmware wants the RAM back: its client goes too.
    if (user == User::Presence) ForgetKept(g_presenceClient);
    const bool quiet = user == User::Presence;
    std::lock_guard<std::mutex> lock(g_connectMutex);
    g_holders &= static_cast<uint8_t>(~Bit(user));
    if (g_holders != 0)
    {
        ApplyPowerSave();
        return;
    }
    if (g_wifi.events == nullptr) return;
    TearDown();
    NET_LOG_ROUTINE(quiet, "Disconnected");
    LogHeap("after Disconnect", quiet);
}

bool Internal::Holds(User user)
{
    std::lock_guard<std::mutex> lock(g_connectMutex);
    return (g_holders & Bit(user)) != 0;
}

SendStatus Internal::Send(User user, const Request& request, Response& response, Progress progress,
                          CancelCheck cancelled, void* context)
{
    std::atomic<int>& count = g_sending[Bit(user)];
    count.fetch_add(1);
    const bool presence = user == User::Presence;
    const SendStatus status =
        SendNow(request, response, progress, cancelled, context, presence, presence ? &g_presenceClient : nullptr);
    count.fetch_sub(1);
    return status;
}

bool Internal::Sending(User user)
{
    return g_sending[Bit(user)].load() > 0;
}

bool Internal::WifiInPsram()
{
    return g_wifiInPsram.load();
}

bool Internal::WifiPsramPossible()
{
    return WifiPsramUsable();
}

ConnectStatus Connect(const LauncherSettings& settings, ConnectReport& report, uint32_t timeoutMs,
                      CancelCheck cancelled, void* context)
{
    return Internal::Connect(Internal::User::Firmware, settings, report, timeoutMs, cancelled, context, false);
}

void Disconnect()
{
    Internal::Disconnect(Internal::User::Firmware);
}

SendStatus Send(const Request& request, Response& response, Progress progress, CancelCheck cancelled, void* context)
{
    return Internal::Send(Internal::User::Firmware, request, response, progress, cancelled, context);
}

// ---------------------------------------------------------------------------------------------------
// The account alone, and the handover

SettingsResult Internal::ReadAccount(LauncherAccount& account, bool& receiveFirmware)
{
    Wipe(account.token, sizeof(account.token));
    account = LauncherAccount{};
    receiveFirmware = true;
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    bool opened = false;
    const SettingsResult result = OpenForReading(opened);
    if (result != SettingsResult::Ok) return result;
    CardenzaHAL::Net::ReadAccount(account);
    receiveFirmware = ReadReceiveFirmware();
    CloseAfterReading(opened);
    return SettingsResult::Ok;
}

namespace {

// Opens "nvs" read-write for the handover: the firmware's own open partition, else the HAL opens it as the
// Launcher does at its start. Never erases. `opened`: the HAL opened it.
bool OpenForWriting(nvs_handle_t& handle, bool& opened)
{
    opened = false;
    nvs_stats_t stats = {};
    if (nvs_get_stats(kNvsLabel, &stats) == ESP_ERR_NVS_NOT_INITIALIZED)
    {
        const esp_err_t err = nvs_flash_init_partition(kNvsLabel);
        if (err != ESP_OK)
        {
            // ESP_ERR_NVS_NO_FREE_PAGES / NEW_VERSION_FOUND: only an erase would open it. Left to the Launcher.
            ESP_LOGE(kTag, "handover: NVS cannot be opened for writing (%s)", esp_err_to_name(err));
            return false;
        }
        opened = true;
    }
    const esp_err_t err = nvs_open_from_partition(kNvsLabel, Presence::Logic::Pending::kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(kTag, "handover: %s cannot be written (%s)", Presence::Logic::Pending::kNamespace, esp_err_to_name(err));
        if (opened) nvs_flash_deinit_partition(kNvsLabel);
        opened = false;
        return false;
    }
    return true;
}

}  // namespace

bool Internal::WritePending(const PendingCommand& pending)
{
    namespace Keys = Presence::Logic::Pending;
    if (pending.command == nullptr || pending.action == nullptr || pending.api == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    nvs_handle_t handle = 0;
    bool opened = false;
    if (!OpenForWriting(handle, opened)) return false;
    const bool unlock = pending.unlock != nullptr && pending.unlock[0] != 0;
    const bool ok = nvs_erase_all(handle) == ESP_OK && nvs_set_u8(handle, Keys::kVersionKey, Keys::kVersion) == ESP_OK &&
                    nvs_set_str(handle, Keys::kCommandKey, pending.command) == ESP_OK &&
                    nvs_set_str(handle, Keys::kActionKey, pending.action) == ESP_OK &&
                    (!unlock || nvs_set_str(handle, Keys::kUnlockKey, pending.unlock) == ESP_OK) &&
                    nvs_set_str(handle, Keys::kApiKey, pending.api) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (opened) nvs_flash_deinit_partition(kNvsLabel);
    if (!ok) ESP_LOGE(kTag, "handover: writing %s failed", Keys::kNamespace);
    return ok;
}

bool Internal::ClearPending()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    nvs_handle_t handle = 0;
    bool opened = false;
    if (!OpenForWriting(handle, opened)) return false;
    const bool ok = nvs_erase_all(handle) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (opened) nvs_flash_deinit_partition(kNvsLabel);
    return ok;
}

namespace {

bool ReadPartitionAt(void* context, uint32_t offset, void* out, uint32_t length)
{
    return esp_partition_read(static_cast<const esp_partition_t*>(context), offset, out, length) == ESP_OK;
}

}  // namespace

bool RunningImageDigest(char hex[65])
{
    // The running image does not change: its digest is read once. From its headers alone (a dozen reads
    // of a few bytes, Presence::Logic::AppendedDigestOffset): the bootloader checked the image when it
    // started it, and reading all of it (esp_partition_get_sha256) would pull a megabyte or two through
    // the flash cache the firmware's code runs from.
    static char cached[65];
    if (cached[0] == 0)
    {
        const esp_partition_t* running = esp_ota_get_running_partition();
        uint32_t offset = 0;
        uint8_t digest[32];
        if (running == nullptr ||
            !Presence::Logic::AppendedDigestOffset(ReadPartitionAt, const_cast<esp_partition_t*>(running), running->size,
                                                   offset) ||
            esp_partition_read(running, offset, digest, sizeof(digest)) != ESP_OK)
        {
            hex[0] = 0;
            return false;
        }
        char text[65];
        for (std::size_t i = 0; i < sizeof(digest); ++i) std::snprintf(text + 2 * i, 3, "%02x", digest[i]);
        std::memcpy(cached, text, sizeof(cached));
    }
    std::memcpy(hex, cached, sizeof(cached));
    return true;
}

}  // namespace CardenzaHAL::Net

#if CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
// mbedTLS's memory: PSRAM when the heap has some (record buffers, certificates, handshake), internal
// otherwise. mbedTLS's hardware AES and SHA on the ESP32-S3 take PSRAM buffers (their DMA reaches PSRAM;
// the drivers write back and invalidate the cache around it, or bounce through internal blocks).
// Defined here, beside Send, so that they are linked whenever Net is; installed by UseTlsAllocator.
extern "C" void* esp_mbedtls_mem_calloc(size_t n, size_t size)
{
    if (void* block = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) return block;
    return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

extern "C" void esp_mbedtls_mem_free(void* p)
{
    heap_caps_free(p);
}
#endif
