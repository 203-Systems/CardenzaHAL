// The system settings (system/settings.h) on the device: read from the NVS partition at the start without
// writing anything, applied to the codec, the backlight and the key lights, and written back by a task of
// the HAL's own when they have rested (system/settings_logic.h WritePlan).
//
// Reading: the partition through the cache (esp_partition_mmap), page by page with the system menu's NVS
// reader (system/menu/logic.h), as the Launcher's values have always been read here: no nvs_flash_init at
// the start (it may repair and write), nothing in RAM afterwards.
//
// Writing (docs/SETTINGS.md): NVS opened read-write as Net's handover opens it (nvs_flash_init_partition when
// nobody has it open, never an erase: a partition that would need one keeps the settings for this run), the
// keys that changed set and committed, the partition closed again if it was opened here. A flash write
// stalls both cores' caches for as long as it lasts (a page erase is tens of milliseconds): the speaker is
// faded out first (the codec's volume, Audio::detail::Duck) and the key lights held dark (their scan
// interrupt is not in IRAM and would leave one row lit), and both come back after.
#include "system/settings.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include <esp_log.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "audio/audio.h"
#include "audio/internal/duck.h"
#include "display/internal/system_brightness.h"
#include "keyboard/internal/flash_guard.h"
#include "keyboard/keyboard.h"
#include "system/internal/nvs_access.h"
#include "system/internal/settings_hooks.h"
#include "system/menu.h"
#include "system/menu/logic.h"
#include "system/settings_logic.h"

namespace CardenzaHAL::Settings {

namespace {

using detail::Index;
using detail::Values;

constexpr char kTag[] = "CardenzaHAL";
constexpr char kNvsLabel[] = "nvs";
// NVS's own initialisation and a write: internal RAM only while the writer exists.
constexpr uint32_t kWriterStack = 4096;
constexpr UBaseType_t kWriterPriority = 2;
constexpr uint32_t kWriterPollMs = 100;
constexpr uint32_t kWriterIdleMs = 3000;  // then the writer goes
constexpr uint32_t kFlushWaitMs = 1500;

std::mutex g_mutex;  // g_values, g_plan, g_listener
Values g_values = detail::Defaults();
detail::WritePlan g_plan;
Listener g_listener = nullptr;
void* g_listenerContext = nullptr;
bool g_loaded = false;
bool g_unwritable = false;  // NVS could not be opened without an erase: this run only

std::atomic<bool> g_writerRunning{false};
std::atomic<bool> g_flush{false};
SemaphoreHandle_t g_flushed = nullptr;  // given after each write attempt a Flush waits for

uint32_t NowMs()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

// ---- Reading ---------------------------------------------------------------------------------------------

struct Read {
    Values values = detail::Defaults();
    uint32_t found = 0;
};

Read ReadFlash()
{
    Read read;
    const esp_partition_t* nvs = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, kNvsLabel);
    if (nvs == nullptr) return read;
    const void* mapped = nullptr;
    esp_partition_mmap_handle_t handle = 0;
    if (esp_partition_mmap(nvs, 0, nvs->size, ESP_PARTITION_MMAP_DATA, &mapped, &handle) != ESP_OK)
    {
        ESP_LOGW(kTag, "settings: the NVS partition cannot be read: defaults");
        return read;
    }
    const auto* bytes = static_cast<const uint8_t*>(mapped);
    const std::size_t pages = nvs->size / SystemMenu::detail::kNvsPageBytes;
    uint8_t ours = 0, launcher = 0;
    for (std::size_t p = 0; p < pages; ++p)
    {
        const uint8_t* page = bytes + p * SystemMenu::detail::kNvsPageBytes;
        if (ours == 0) ours = SystemMenu::detail::NvsNamespaceIndex(page, detail::kNamespace);
        if (launcher == 0) launcher = SystemMenu::detail::NvsNamespaceIndex(page, detail::kLauncherNamespace);
    }
    for (int i = 0; i < kCount; ++i)
    {
        const detail::Stored& stored = detail::kStored[i];
        const uint8_t index = std::strcmp(stored.nameSpace, detail::kNamespace) == 0 ? ours : launcher;
        if (index == 0) continue;
        for (std::size_t p = 0; p < pages; ++p)
        {
            const uint8_t* page = bytes + p * SystemMenu::detail::kNvsPageBytes;
            int32_t value = 0;
            bool found = false;
            if (stored.type == detail::kTypeU8)
            {
                uint8_t u8 = 0;
                found = SystemMenu::detail::NvsFindU8(page, index, stored.key, u8);
                value = u8;
            }
            else
                found = SystemMenu::detail::NvsFindI32(page, index, stored.key, value);
            if (!found) continue;
            read.values[i] = detail::FromStored(detail::IdAt(i), value);
            read.found |= 1u << i;
            break;
        }
    }
    esp_partition_munmap(handle);
    // A volume above a limit read from elsewhere (an older writer): the limit wins, as Set would have it.
    read.values[Index(Id::Volume)] = std::min(read.values[Index(Id::Volume)], read.values[Index(Id::VolumeLimit)]);
    return read;
}

// ---- Applying --------------------------------------------------------------------------------------------

void ApplyOne(Id id, int value)
{
    switch (id)
    {
        case Id::Volume: (void)Audio::SetVolume(detail::CodecPercent(value)); break;
        case Id::VolumeLimit: break;  // through Volume
        case Id::ScreenBrightness: Display::detail::SetSystemBrightness(value); break;
        case Id::KeyBrightness: Keyboard::setLEDGlobalBrightness(static_cast<uint8_t>(value)); break;
        case Id::Language: break;  // the menu's words; the firmware's own, through Get
        case Id::WebUsbLanding:
        case Id::WebUsbSeen: break;  // the USB device's descriptors, the next time a computer reads them
        case Id::ScreenshotUpload: break;  // read by the screenshots' uploads (screenshot/screenshot.cpp)
    }
}

// Applies and tells the listener what `changed` (a bit per id) holds, outside the lock.
void Changed(uint32_t changed, const Values& values)
{
    if (changed == 0) return;
    Listener listener;
    void* context;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        listener = g_listener;
        context = g_listenerContext;
    }
    for (int i = 0; i < kCount; ++i)
        if ((changed >> i) & 1) ApplyOne(detail::IdAt(i), values[i]);
    if (listener == nullptr) return;
    for (int i = 0; i < kCount; ++i)
        if ((changed >> i) & 1) listener(detail::IdAt(i), values[i], context);
}

// ---- Writing ---------------------------------------------------------------------------------------------

// Sets one namespace's keys of `dirty` (a bit per id). The ids written.
uint32_t WriteNamespace(const char* nameSpace, uint32_t dirty, const Values& values)
{
    uint32_t mine = 0;
    for (int i = 0; i < kCount; ++i)
        if (((dirty >> i) & 1) && std::strcmp(detail::kStored[i].nameSpace, nameSpace) == 0) mine |= 1u << i;
    if (mine == 0) return 0;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open_from_partition(kNvsLabel, nameSpace, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(kTag, "settings: %s cannot be written (%s)", nameSpace, esp_err_to_name(err));
        return 0;
    }
    bool ok = true;
    for (int i = 0; i < kCount && ok; ++i)
    {
        if (!((mine >> i) & 1)) continue;
        const detail::Stored& stored = detail::kStored[i];
        err = stored.type == detail::kTypeU8 ? nvs_set_u8(handle, stored.key, static_cast<uint8_t>(values[i]))
                                             : nvs_set_i32(handle, stored.key, values[i]);
        ok = err == ESP_OK;
    }
    if (ok) ok = (err = nvs_commit(handle)) == ESP_OK;
    nvs_close(handle);
    if (!ok)
    {
        ESP_LOGE(kTag, "settings: writing %s failed (%s)", nameSpace, esp_err_to_name(err));
        return 0;
    }
    return mine;
}

// Writes `dirty` of `values`: the speaker faded out and the key lights dark around it. The ids written.
uint32_t WriteFlash(uint32_t dirty, const Values& values)
{
    std::lock_guard<std::mutex> nvsLock(System::detail::NvsWriteMutex());
    bool opened = false;
    nvs_stats_t stats = {};
    if (nvs_get_stats(kNvsLabel, &stats) == ESP_ERR_NVS_NOT_INITIALIZED)
    {
        // Opening reads every page (and may repair one): quiet too.
        Audio::detail::Duck(true);
        Keyboard::detail::HoldLightsDark(true);
        const esp_err_t err = nvs_flash_init_partition(kNvsLabel);
        if (err != ESP_OK)
        {
            Keyboard::detail::HoldLightsDark(false);
            Audio::detail::Duck(false);
            // ESP_ERR_NVS_NO_FREE_PAGES / NEW_VERSION_FOUND: only an erase would open it; that is the Launcher's.
            ESP_LOGE(kTag, "settings: NVS cannot be opened for writing (%s): kept for this run only", esp_err_to_name(err));
            g_unwritable = true;
            return 0;
        }
        opened = true;
    }
    else
    {
        Audio::detail::Duck(true);
        Keyboard::detail::HoldLightsDark(true);
    }
    const int64_t start = esp_timer_get_time();
    uint32_t written = WriteNamespace(detail::kNamespace, dirty, values);
    written |= WriteNamespace(detail::kLauncherNamespace, dirty, values);
    if (opened) nvs_flash_deinit_partition(kNvsLabel);
    Keyboard::detail::HoldLightsDark(false);
    Audio::detail::Duck(false);
    ESP_LOGI(kTag, "settings: written in %lld ms", static_cast<long long>((esp_timer_get_time() - start) / 1000));
    return written;
}

// One write if one is due. True if something was written or tried.
bool WriteIfDue(bool flush)
{
    Values values;
    uint32_t dirty;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_unwritable || !g_plan.Due(g_values, NowMs(), SystemMenu::IsOpen(), flush)) return false;
        values = g_values;
        dirty = g_plan.Dirty(values);
    }
    const uint32_t written = WriteFlash(dirty, values);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_plan.Written(values, written);
    if (written != dirty && !g_unwritable) g_plan.Changed(NowMs());  // try again after a rest
    return true;
}

void Writer(void*)
{
    uint32_t idleSince = NowMs();
    for (;;)
    {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kWriterPollMs));
        const bool flush = g_flush.exchange(false);
        const bool wrote = WriteIfDue(flush);
        if (flush && g_flushed != nullptr) xSemaphoreGive(g_flushed);
        bool pending;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            pending = !g_unwritable && g_plan.Dirty(g_values) != 0;
        }
        if (wrote || pending) idleSince = NowMs();
        if (!pending && NowMs() - idleSince >= kWriterIdleMs)
        {
            g_writerRunning.store(false);
            // A change that came just now finds no writer running and starts one.
            bool again;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                again = !g_unwritable && g_plan.Dirty(g_values) != 0;
            }
            if (!again || g_writerRunning.exchange(true)) break;
        }
    }
    vTaskDelete(nullptr);
}

TaskHandle_t g_writer = nullptr;

void WakeWriter()
{
    if (!g_writerRunning.exchange(true))
    {
        if (xTaskCreate(Writer, "sys_settings", kWriterStack, nullptr, kWriterPriority, &g_writer) != pdPASS)
        {
            g_writerRunning.store(false);
            ESP_LOGE(kTag, "settings: no memory for the writer");
        }
        return;
    }
    if (g_writer != nullptr) xTaskNotifyGive(g_writer);
}

void OnShutdown()
{
    (void)Flush();
}

}  // namespace

const Info& Describe(Id id)
{
    return detail::kInfo[std::clamp(Index(id), 0, kCount - 1)];
}

int Get(Id id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_values[std::clamp(Index(id), 0, kCount - 1)];
}

int Set(Id id, int value)
{
    if (Index(id) < 0 || Index(id) >= kCount) return 0;
    uint32_t changed;
    Values values;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        changed = detail::Apply(g_values, id, value);
        g_plan.MarkSet(id);
        if (changed != 0) g_plan.Changed(NowMs());
        values = g_values;
    }
    Changed(changed, values);
    if (changed != 0) WakeWriter();
    return values[Index(id)];
}

void Reset()
{
    uint32_t changed;
    Values values;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        changed = detail::ApplyReset(g_values);
        if (changed != 0) g_plan.Changed(NowMs());
        values = g_values;
    }
    ESP_LOGI(kTag, "settings: reset to the defaults");
    Changed(changed, values);
    if (changed != 0) WakeWriter();
}

bool Stored(Id id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_plan.Stored(id);
}

bool Saved()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_unwritable || g_plan.Dirty(g_values) == 0;
}

bool Flush()
{
    if (Saved()) return true;
    if (g_flushed == nullptr)
    {
        SemaphoreHandle_t made = xSemaphoreCreateBinary();
        if (made == nullptr) return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_flushed == nullptr) g_flushed = made;
        else vSemaphoreDelete(made);
    }
    (void)xSemaphoreTake(g_flushed, 0);
    g_flush.store(true);
    WakeWriter();
    (void)xSemaphoreTake(g_flushed, pdMS_TO_TICKS(kFlushWaitMs));
    return Saved();
}

void SetListener(Listener listener, void* context)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_listener = listener;
    g_listenerContext = context;
}

namespace detail {

void Load()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_loaded) return;
        g_loaded = true;
    }
    const Read read = ReadFlash();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_values = read.values;
        g_plan.Loaded(read.values, read.found);
    }
    // Audio::SetVolume before Audio::Init only caches the level: the codec starts at it.
    (void)Audio::SetVolume(CodecPercent(read.values[Index(Id::Volume)]));
    Display::detail::SetSystemBrightness(read.values[Index(Id::ScreenBrightness)]);
    (void)esp_register_shutdown_handler(OnShutdown);
    ESP_LOGI(kTag, "settings: volume %d (limit %d), screen %d%%, key lights %d, language %d", read.values[0],
             read.values[1], read.values[2], read.values[3], read.values[4]);
}

void ApplyAfterInit()
{
    Values values;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        values = g_values;
    }
    (void)Audio::SetVolume(CodecPercent(values[Index(Id::Volume)]));
    Display::detail::SetSystemBrightness(values[Index(Id::ScreenBrightness)]);
    Keyboard::setLEDGlobalBrightness(static_cast<uint8_t>(values[Index(Id::KeyBrightness)]));
}

void MenuClosed()
{
    bool dirty;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        dirty = !g_unwritable && g_plan.Dirty(g_values) != 0;
    }
    if (!dirty) return;
    g_flush.store(true);
    WakeWriter();
}


}  // namespace detail

}  // namespace CardenzaHAL::Settings
