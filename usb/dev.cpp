// The development channel: a computer's questions about the firmware, answered by the HAL (docs/usb-dev.md).
//
// Nothing of it runs until a computer asks. The USB task hands each request to a worker task (priority 1,
// created on demand, gone 10 s after the last request) through a queue made on the first request; the worker
// answers through usb.cpp, which waits for room as the computer reads. Flash is read through the cache
// (memory-mapped), never with the cache disabled; the only writes are the ones asked for (card files, erasing
// the core dump).
#include "usb/internal/dev.h"

#include <sdkconfig.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "usb/dev_logic.h"

#if CONFIG_CARDENZA_HAL_USB_DEV
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <driver/i2c_master.h>
#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_rom_sys.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <spi_flash_mmap.h>
#if CONFIG_SPIRAM
#include <esp_psram.h>
#endif
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
#include <esp_core_dump.h>
#define CARDENZA_DEV_COREDUMP_SUMMARY 1
#endif

#include "board/pins.h"
#include "cardenzaHAL.h"
#include "display/internal/overlay.h"
#include "display/internal/snapshot.h"
#include "keyboard/internal/inject.h"
#include "keyboard/internal/overlay.h"
#include "system/internal/hooks.h"
#include "system/menu.h"
#include "usb/usb.h"
#if CONFIG_CARDENZA_HAL_PRESENCE
#include "net/presence.h"
#endif
#endif  // CONFIG_CARDENZA_HAL_USB_DEV

// Set by CMakeLists.txt when the build is configured.
#ifndef CARDENZA_HAL_COMMIT
#define CARDENZA_HAL_COMMIT ""
#endif
#ifndef CARDENZA_HAL_DIRTY
#define CARDENZA_HAL_DIRTY 0
#endif
#ifndef CARDENZA_FIRMWARE_CODE
#define CARDENZA_FIRMWARE_CODE ""
#endif

namespace CardenzaHAL::USB::detail {

namespace {

using namespace CardenzaHAL::USB::Dev;

bool SendMessage(uint8_t id, bool sysex, uint16_t seq, uint8_t flags, const uint8_t* payload, std::size_t length, uint32_t waitMs)
{
    if (sysex)
    {
        uint8_t message[kSysexMessage];
        const std::size_t size = SysexAnswer(id, seq, flags, payload, length, message);
        return DevSendSysex(message, size, waitMs);
    }
    uint8_t report[kReport];
    HidAnswer(id, seq, flags, payload, length, report);
    return DevSendHid(report, waitMs);
}

// An error answer of one message (cut to fit), at once: from the USB task, which cannot wait.
void AnswerNow(uint8_t id, bool sysex, const char* text)
{
    const std::size_t length = std::min(std::strlen(text), sysex ? kSysexPayload : kHidPayload);
    SendMessage(id, sysex, 0, kFlagLast | kFlagError, reinterpret_cast<const uint8_t*>(text), length, 0);
}

}  // namespace

#if !CONFIG_CARDENZA_HAL_USB_DEV

void DevRequest(const uint8_t* data, std::size_t length, bool sysex)
{
    if (length >= 2 && (data[1] & 0x7F) != kCancel) AnswerNow(data[0] & 0x7F, sysex, "off");
}

void DevHostGone() {}

#else

namespace {

constexpr uint32_t kIdleMs = 10000;       // the worker goes this long after the last request
constexpr UBaseType_t kQueueDepth = 8;
constexpr uint32_t kStackBytes = 6144;    // FAT and snprintf; internal RAM, only while the worker exists
constexpr uint32_t kSendWaitMs = 50;      // for room for each report or USB-MIDI packet: then the computer is gone
constexpr uint32_t kSnapshotWaitMs = 250; // for the firmware's next frame
constexpr int kHomeKey = 57;
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
constexpr bool kCoredumpEnabled = true;  // the firmware writes a dump when it crashes
#else
constexpr bool kCoredumpEnabled = false;
#endif

struct Request {
    uint8_t id = 0;
    uint8_t op = 0;
    bool sysex = false;
    uint8_t length = 0;
    uint8_t args[kMaxArgs] = {};
};

QueueHandle_t g_queue = nullptr;  // made on the first request, kept
std::atomic<bool> g_running{false};
std::atomic<bool> g_cancel{false};
std::atomic<bool> g_homeDown{false};

void ReleaseKeys()
{
    Keyboard::detail::ReleaseInjectedKeys();
    if (g_homeDown.exchange(false)) System::detail::InjectHome(false);
}

// One answer: messages of up to a payload each, numbered; the last one says so.
class Stream {
  public:
    Stream(uint8_t id, bool sysex) : id_(id), sysex_(sysex) {}
    ~Stream()
    {
        if (!done_) Fail("no answer");
    }

    bool Write(const void* data, std::size_t length)
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        while (length != 0 && !done_)
        {
            const std::size_t room = Chunk() - used_;
            const std::size_t take = std::min(room, length);
            std::memcpy(buffer_ + used_, bytes, take);
            used_ += take;
            bytes += take;
            length -= take;
            if (used_ == Chunk() && !Send(0)) return false;
        }
        return !done_;
    }
    bool Finish()
    {
        if (done_) return false;
        const bool sent = Send(kFlagLast);
        done_ = true;
        return sent;
    }
    bool Fail(const char* text)
    {
        if (done_) return false;
        done_ = true;
        const std::size_t length = std::min(std::strlen(text), Chunk());
        return SendMessage(id_, sysex_, seq_, kFlagLast | kFlagError, reinterpret_cast<const uint8_t*>(text), length, kSendWaitMs);
    }
    bool Ok() const { return !done_; }

  private:
    std::size_t Chunk() const { return sysex_ ? kSysexPayload : kHidPayload; }
    bool Send(uint8_t flags)
    {
        if (g_cancel.load(std::memory_order_relaxed))
        {
            Fail("cancelled");
            return false;
        }
        if (!SendMessage(id_, sysex_, seq_, flags, buffer_, used_, kSendWaitMs))
        {
            done_ = true;  // the computer stopped reading
            return false;
        }
        seq_ = static_cast<uint16_t>((seq_ + 1) & 0x3FFF);
        used_ = 0;
        return true;
    }

    uint8_t id_;
    bool sysex_;
    uint16_t seq_ = 0;
    uint8_t buffer_[kHidPayload] = {};
    std::size_t used_ = 0;
    bool done_ = false;
};

struct StreamSink {
    Stream& stream;
    bool operator()(const char* text, std::size_t length) { return stream.Write(text, length); }
};
using Writer = Json<StreamSink>;

// Flash through the cache, 64 KB at a time (the MMU's page): never with the cache disabled.
class FlashReader {
  public:
    ~FlashReader() { Unmap(); }
    bool Read(uint32_t address, void* out, std::size_t length)
    {
        auto* bytes = static_cast<uint8_t*>(out);
        while (length != 0)
        {
            const uint32_t block = address & ~(kBlock - 1);
            if (block != base_ && !Map(block)) return false;
            const std::size_t take = std::min<std::size_t>(length, block + kBlock - address);
            std::memcpy(bytes, mapped_ + (address - block), take);
            bytes += take;
            address += static_cast<uint32_t>(take);
            length -= take;
        }
        return true;
    }

  private:
    static constexpr uint32_t kBlock = 0x10000;
    bool Map(uint32_t block)
    {
        Unmap();
        const void* pointer = nullptr;
        if (spi_flash_mmap(block, kBlock, SPI_FLASH_MMAP_DATA, &pointer, &handle_) != ESP_OK) return false;
        mapped_ = static_cast<const uint8_t*>(pointer);
        base_ = block;
        return true;
    }
    void Unmap()
    {
        if (mapped_ != nullptr) spi_flash_munmap(handle_);
        mapped_ = nullptr;
        base_ = UINT32_MAX;
    }
    spi_flash_mmap_handle_t handle_ = 0;
    const uint8_t* mapped_ = nullptr;
    uint32_t base_ = UINT32_MAX;
};

// PSRAM only (or nothing): the channel never takes the firmware's internal RAM for its buffers.
void* SlowAlloc(std::size_t bytes) { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

const char* ResetName(esp_reset_reason_t reason)
{
    switch (reason)
    {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_EXT: return "external";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt-watchdog";
        case ESP_RST_TASK_WDT: return "task-watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep-sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        case ESP_RST_USB: return "usb";
        case ESP_RST_JTAG: return "jtag";
        case ESP_RST_EFUSE: return "efuse";
        case ESP_RST_PWR_GLITCH: return "power-glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu-lockup";
        default: return "unknown";
    }
}

const char* WakeupName(esp_sleep_wakeup_cause_t cause)
{
    switch (cause)
    {
        case ESP_SLEEP_WAKEUP_EXT0: return "ext0";
        case ESP_SLEEP_WAKEUP_EXT1: return "ext1";
        case ESP_SLEEP_WAKEUP_TIMER: return "timer";
        case ESP_SLEEP_WAKEUP_TOUCHPAD: return "touchpad";
        case ESP_SLEEP_WAKEUP_ULP: return "ulp";
        case ESP_SLEEP_WAKEUP_GPIO: return "gpio";
        case ESP_SLEEP_WAKEUP_UART: return "uart";
        default: return "other";
    }
}

void Hex(const uint8_t* bytes, std::size_t length, char* out)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < length; ++index)
    {
        out[2 * index] = kDigits[bytes[index] >> 4];
        out[2 * index + 1] = kDigits[bytes[index] & 15];
    }
    out[2 * length] = 0;
}

// The 203 Launcher's own description, from its image at 0x10000 (its slot on every Cardenza), unless that is
// this firmware.
bool LauncherDescription(esp_app_desc_t& out)
{
    constexpr uint32_t kLauncherSlot = 0x10000;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running != nullptr && running->address == kLauncherSlot) return false;
    FlashReader flash;
    uint8_t magic = 0;
    if (!flash.Read(kLauncherSlot, &magic, 1) || magic != 0xE9) return false;
    // The image header (24 bytes) and the first segment's header (8): then the description.
    if (!flash.Read(kLauncherSlot + 32, &out, sizeof(out))) return false;
    return out.magic_word == ESP_APP_DESC_MAGIC_WORD;
}

// ---- INFO

void Info(Stream& stream)
{
    StreamSink sink{stream};
    Writer json(sink);
    const esp_app_desc_t* app = esp_app_get_description();
    char hash[65];
    Hex(app->app_elf_sha256, sizeof(app->app_elf_sha256), hash);
    char built[40];
    std::snprintf(built, sizeof(built), "%.16s %.16s", app->date, app->time);
    json.Object().Key("protocol").Int(kProtocolVersion);
    json.Key("firmware").Object();
    json.Key("name").Str(app->project_name, strnlen(app->project_name, sizeof(app->project_name)));
    json.Key("version").Str(app->version, strnlen(app->version, sizeof(app->version)));
    json.Key("code");
    if (CARDENZA_FIRMWARE_CODE[0] != 0) json.Str(CARDENZA_FIRMWARE_CODE); else json.Null();
    json.Key("product").Str(Product());
    json.Key("idf").Str(app->idf_ver, strnlen(app->idf_ver, sizeof(app->idf_ver)));
    json.Key("built").Str(built);
    json.Key("elf_sha256").Str(hash);
    json.Key("secure_version").Uint(app->secure_version);
    json.End();
    json.Key("hal").Object().Key("commit").Str(CARDENZA_HAL_COMMIT).Key("dirty").Bool(CARDENZA_HAL_DIRTY != 0).End();
    json.Key("launcher");
    esp_app_desc_t launcher = {};
    if (LauncherDescription(launcher))
        json.Object()
            .Key("name").Str(launcher.project_name, strnlen(launcher.project_name, sizeof(launcher.project_name)))
            .Key("version").Str(launcher.version, strnlen(launcher.version, sizeof(launcher.version)))
            .End();
    else
        json.Null();
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    char revision[12];
    std::snprintf(revision, sizeof(revision), "%u.%u", chip.revision / 100u, chip.revision % 100u);
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macText[18];
    std::snprintf(macText, sizeof(macText), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    uint32_t flashBytes = 0;
    esp_flash_get_size(nullptr, &flashBytes);
    std::size_t psramBytes = 0;
#if CONFIG_SPIRAM
    if (esp_psram_is_initialized()) psramBytes = esp_psram_get_size();
#endif
    json.Key("chip").Object();
    json.Key("model").Str(chip.model == CHIP_ESP32S3 ? "ESP32-S3" : "ESP32");
    json.Key("revision").Str(revision);
    json.Key("cores").Int(chip.cores);
    json.Key("cpu_mhz").Uint(esp_rom_get_cpu_ticks_per_us());
    json.Key("mac").Str(macText);
    json.Key("flash_bytes").Uint(flashBytes);
    json.Key("psram_bytes").Uint(psramBytes);
    json.End();
    json.Key("uptime_ms").Uint(static_cast<unsigned long long>(esp_timer_get_time() / 1000));
    const esp_reset_reason_t reason = esp_reset_reason();
    json.Key("reset").Str(ResetName(reason));
    json.Key("wakeup");
    if (reason == ESP_RST_DEEPSLEEP) json.Str(WakeupName(esp_sleep_get_wakeup_cause())); else json.Null();
    const InitReport& init = LastInitReport();
    json.Key("hal_init").Object()
        .Key("system").Bool(init.system).Key("keyboard").Bool(init.keyboard).Key("audio").Bool(init.audio)
        .Key("display").Bool(init.display).Key("storage").Bool(init.storage).Key("ir").Bool(init.ir)
        .End();
    json.End();
    if (json.Ok()) stream.Finish();
}

// ---- PARTITIONS

const char* SubtypeName(const esp_partition_t& partition, char* buffer, std::size_t capacity)
{
    if (partition.type == ESP_PARTITION_TYPE_APP)
    {
        if (partition.subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) return "factory";
        if (partition.subtype == ESP_PARTITION_SUBTYPE_APP_TEST) return "test";
        if (partition.subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && partition.subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX)
        {
            std::snprintf(buffer, capacity, "ota_%d", partition.subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
            return buffer;
        }
    }
    else if (partition.type == ESP_PARTITION_TYPE_DATA)
    {
        switch (partition.subtype)
        {
            case ESP_PARTITION_SUBTYPE_DATA_OTA: return "ota";
            case ESP_PARTITION_SUBTYPE_DATA_PHY: return "phy";
            case ESP_PARTITION_SUBTYPE_DATA_NVS: return "nvs";
            case ESP_PARTITION_SUBTYPE_DATA_COREDUMP: return "coredump";
            case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS: return "nvs_keys";
            case ESP_PARTITION_SUBTYPE_DATA_EFUSE_EM: return "efuse";
            case ESP_PARTITION_SUBTYPE_DATA_ESPHTTPD: return "esphttpd";
            case ESP_PARTITION_SUBTYPE_DATA_FAT: return "fat";
            case ESP_PARTITION_SUBTYPE_DATA_SPIFFS: return "spiffs";
            case ESP_PARTITION_SUBTYPE_DATA_LITTLEFS: return "littlefs";
            default: break;
        }
    }
    std::snprintf(buffer, capacity, "0x%02x", partition.subtype);
    return buffer;
}

// An image's length in a partition, through the cache.
uint32_t ImageLength(FlashReader& flash, const esp_partition_t& partition)
{
    return AppImageLength([&](uint32_t offset, void* out, std::size_t length) { return flash.Read(partition.address + offset, out, length); },
                          partition.size);
}

void Partitions(Stream& stream)
{
    constexpr std::size_t kMax = 32;
    const esp_partition_t* table[kMax];
    std::size_t count = 0;
    for (esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr); it != nullptr;
         it = esp_partition_next(it))
    {
        if (count == kMax)
        {
            esp_partition_iterator_release(it);
            break;
        }
        table[count++] = esp_partition_get(it);
    }
    std::sort(table, table + count, [](const esp_partition_t* a, const esp_partition_t* b) { return a->address < b->address; });
    const esp_partition_t* running = esp_ota_get_running_partition();
    uint32_t flashBytes = 0;
    esp_flash_get_size(nullptr, &flashBytes);

    StreamSink sink{stream};
    Writer json(sink);
    FlashReader flash;
    json.Object().Key("flash_bytes").Uint(flashBytes);
    json.Key("running");
    if (running != nullptr) json.Str(running->label); else json.Null();
    json.Key("partitions").Array();
    uint32_t end = 0;
    for (std::size_t index = 0; index < count && json.Ok(); ++index)
    {
        const esp_partition_t& partition = *table[index];
        end = std::max<uint32_t>(end, partition.address + partition.size);
        char subtype[12];
        json.Object();
        json.Key("label").Str(partition.label);
        json.Key("type");
        if (partition.type == ESP_PARTITION_TYPE_APP) json.Str("app");
        else if (partition.type == ESP_PARTITION_TYPE_DATA) json.Str("data");
        else
        {
            char type[8];
            std::snprintf(type, sizeof(type), "0x%02x", partition.type);
            json.Str(type);
        }
        json.Key("subtype").Str(SubtypeName(partition, subtype, sizeof(subtype)));
        json.Key("offset").Uint(partition.address);
        json.Key("size").Uint(partition.size);
        if (partition.encrypted) json.Key("encrypted").Bool(true);
        // An application, and the Launcher's slot (data 0x40 in the HAL's table).
        if (partition.type == ESP_PARTITION_TYPE_APP || (partition.type == ESP_PARTITION_TYPE_DATA && partition.subtype == 0x40))
        {
            const uint32_t image = ImageLength(flash, partition);
            json.Key("image");
            if (image != 0) json.Uint(image); else json.Null();
        }
        if (partition.type == ESP_PARTITION_TYPE_DATA && partition.subtype == ESP_PARTITION_SUBTYPE_DATA_NVS)
        {
            uint32_t pages = partition.size / 4096, used = 0;
            for (uint32_t page = 0; page < pages; ++page)
            {
                uint8_t header[64];
                if (!flash.Read(partition.address + page * 4096, header, sizeof(header))) break;
                used += ReadNvsPage(header).written;
            }
            json.Key("nvs").Object().Key("pages").Uint(pages).Key("entries").Uint(pages * kNvsEntriesPerPage).Key("used").Uint(used).End();
        }
        if (running != nullptr && partition.address == running->address) json.Key("running").Bool(true);
        json.End();
    }
    json.End(']');
    json.Key("free_bytes").Uint(flashBytes > end ? flashBytes - end : 0);
    json.End();
    if (json.Ok()) stream.Finish();
}

// ---- MEMORY

void Heap(Writer& json, const char* key, uint32_t caps)
{
    const std::size_t total = heap_caps_get_total_size(caps);
    json.Key(key);
    if (total == 0)
    {
        json.Null();
        return;
    }
    multi_heap_info_t info = {};
    heap_caps_get_info(&info, caps);
    json.Object().Key("total").Uint(total).Key("free").Uint(info.total_free_bytes).Key("min_free").Uint(info.minimum_free_bytes)
        .Key("largest").Uint(info.largest_free_block).End();
}

#if configUSE_TRACE_FACILITY
const char* TaskStateName(eTaskState state)
{
    switch (state)
    {
        case eRunning: return "running";
        case eReady: return "ready";
        case eBlocked: return "blocked";
        case eSuspended: return "suspended";
        case eDeleted: return "deleted";
        default: return "unknown";
    }
}

// Every task's status (in PSRAM when there is some, else internal RAM for a moment); null when none fits.
TaskStatus_t* SampleTasks(UBaseType_t& count, configRUN_TIME_COUNTER_TYPE& total)
{
    const UBaseType_t capacity = uxTaskGetNumberOfTasks() + 4;
    const std::size_t bytes = capacity * sizeof(TaskStatus_t);
    auto* tasks = static_cast<TaskStatus_t*>(SlowAlloc(bytes));
    if (tasks == nullptr) tasks = static_cast<TaskStatus_t*>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (tasks == nullptr) return nullptr;
    total = 0;
    count = uxTaskGetSystemState(tasks, capacity, &total);
    return tasks;
}
#endif

void Memory(Stream& stream, const uint8_t* args, std::size_t length)
{
    StreamSink sink{stream};
    Writer json(sink);
    json.Object();
    Heap(json, "internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Heap(json, "psram", MALLOC_CAP_SPIRAM);
    json.Key("dma").Object().Key("free").Uint(heap_caps_get_free_size(MALLOC_CAP_DMA))
        .Key("largest").Uint(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)).End();
    json.Key("tasks");
#if configUSE_TRACE_FACILITY
    UBaseType_t count = 0;
    configRUN_TIME_COUNTER_TYPE total = 0;
    TaskStatus_t* tasks = SampleTasks(count, total);
    uint32_t windowMs = 0;
#if configGENERATE_RUN_TIME_STATS
    UBaseType_t laterCount = 0;
    configRUN_TIME_COUNTER_TYPE laterTotal = 0;
    TaskStatus_t* later = nullptr;
    if (tasks != nullptr)
    {
        windowMs = length >= 1 && args[0] != 0 ? args[0] * 10u : 500u;
        vTaskDelay(pdMS_TO_TICKS(windowMs));
        later = SampleTasks(laterCount, laterTotal);
    }
#endif
    if (tasks == nullptr) json.Null();
    else
    {
        std::sort(tasks, tasks + count, [](const TaskStatus_t& a, const TaskStatus_t& b) {
            return a.uxCurrentPriority != b.uxCurrentPriority ? a.uxCurrentPriority > b.uxCurrentPriority : std::strcmp(a.pcTaskName, b.pcTaskName) < 0;
        });
        json.Array();
        for (UBaseType_t index = 0; index < count && json.Ok(); ++index)
        {
            const TaskStatus_t& task = tasks[index];
            json.Object().Key("name").Str(task.pcTaskName).Key("priority").Uint(task.uxCurrentPriority);
            json.Key("core");
#if defined(configTASKLIST_INCLUDE_COREID) && configTASKLIST_INCLUDE_COREID
            json.Int(task.xCoreID == tskNO_AFFINITY ? -1 : task.xCoreID);
#else
            json.Null();
#endif
            json.Key("stack_free").Uint(task.usStackHighWaterMark);
            json.Key("state").Str(TaskStateName(task.eCurrentState));
            json.Key("cpu");
#if configGENERATE_RUN_TIME_STATS
            const TaskStatus_t* after = nullptr;
            for (UBaseType_t other = 0; later != nullptr && other < laterCount; ++other)
                if (later[other].xHandle == task.xHandle) after = &later[other];
            if (after != nullptr && laterTotal > total)
                json.Fixed(100.0 * static_cast<double>(after->ulRunTimeCounter - task.ulRunTimeCounter) / static_cast<double>(laterTotal - total), 1);
            else
                json.Null();
#else
            json.Null();
#endif
            json.End();
        }
        json.End(']');
        heap_caps_free(tasks);
    }
#if configGENERATE_RUN_TIME_STATS
    if (later != nullptr) heap_caps_free(later);
#endif
    json.Key("cpu_window_ms");
    if (windowMs != 0) json.Uint(windowMs); else json.Null();
#else
    (void)args;
    (void)length;
    json.Null().Key("cpu_window_ms").Null();
#endif
    json.End();
    if (json.Ok()) stream.Finish();
}

// ---- STORAGE, STATUS, I2C

void StorageAnswer(Stream& stream)
{
    StreamSink sink{stream};
    Writer json(sink);
    const bool mounted = Storage::IsMounted();
    uint64_t total = 0, free = 0;
    const bool known = mounted && esp_vfs_fat_info(Storage::kMountPoint, &total, &free) == ESP_OK;
    json.Object().Key("mounted").Bool(mounted).Key("path").Str(Storage::kMountPoint);
    json.Key("card_bytes");
    if (mounted) json.Uint(Storage::GetCapacity()); else json.Null();
    json.Key("total");
    if (known) json.Uint(total); else json.Null();
    json.Key("free");
    if (known) json.Uint(free); else json.Null();
    json.End();
    if (json.Ok()) stream.Finish();
}

void Status(Stream& stream)
{
    StreamSink sink{stream};
    Writer json(sink);
    json.Object();
    json.Key("display").Object().Key("ready").Bool(Display::detail::Ready())
        .Key("framebuffer").Bool(Display::detail::LastFramebuffer() != nullptr)
        .Key("brightness").Uint(Display::GetBrightness()).End();
    Keyboard::Point2D_t keys[16];
    const std::size_t down = Keyboard::detail::OverlayKeys(keys, 16);
    json.Key("keyboard").Object().Key("down").Array();
    for (std::size_t index = 0; index < down; ++index) json.Uint(Keyboard::getKeyNum(keys[index]));
    if (System::detail::HomeButtonRaw()) json.Uint(kHomeKey);
    json.End(']').Key("led_brightness").Uint(Keyboard::ledGlobalBrightness()).End();
    json.Key("menu").Object();
#if CONFIG_CARDENZA_HAL_SYSTEM_MENU
    json.Key("enabled").Bool(SystemMenu::Enabled()).Key("open").Bool(SystemMenu::IsOpen());
#else
    json.Key("enabled").Bool(false).Key("open").Bool(false);
#endif
    json.End();
    json.Key("usb").Object().Key("mounted").Bool(Mounted()).Key("audio_in").Bool(AudioInputActive())
        .Key("audio_out").Bool(AudioOutputActive()).Key("playback_restarts").Uint(AudioStats().playbackRestarts).End();
    json.Key("presence");
#if CONFIG_CARDENZA_HAL_PRESENCE
    static constexpr const char* kStates[] = {"off", "idle", "connecting", "online", "waiting", "signed-out", "handing-over"};
    const auto state = static_cast<std::size_t>(Presence::GetReport().state);
    json.Str(state < sizeof(kStates) / sizeof(kStates[0]) ? kStates[state] : "unknown");
#else
    json.Null();
#endif
    json.End();
    if (json.Ok()) stream.Finish();
}

void I2c(Stream& stream)
{
    i2c_master_bus_handle_t bus = System::GetI2CBus();
    if (bus == nullptr)
    {
        stream.Fail("no I2C bus (System::Init has not run)");
        return;
    }
    StreamSink sink{stream};
    Writer json(sink);
    json.Object().Key("sda").Int(Pins::kExternalI2cSda).Key("scl").Int(Pins::kExternalI2cScl).Key("devices").Array();
    for (uint16_t address = 0x08; address <= 0x77 && json.Ok(); ++address)
    {
        if (i2c_master_probe(bus, address, 20) != ESP_OK) continue;
        json.Object().Key("address").Uint(address).Key("name");
        if (address == 0x08) json.Str("ES8156 (codec)"); else json.Null();
        json.End();
    }
    json.End(']').End();
    if (json.Ok()) stream.Finish();
}

// ---- SCREEN, KEYS

void Screen(Stream& stream)
{
    constexpr std::size_t kPixels = static_cast<std::size_t>(Display::WIDTH) * Display::HEIGHT;
    auto* frame = static_cast<uint16_t*>(SlowAlloc(kPixels * sizeof(uint16_t)));
    const uint16_t* pixels = nullptr;
    uint8_t source = 1;
    if (frame != nullptr)
    {
        Display::detail::RequestSnapshot(frame);
        for (uint32_t waited = 0; waited < kSnapshotWaitMs && pixels == nullptr; waited += 10)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            if (Display::detail::SnapshotTaken()) pixels = frame;
        }
        if (pixels == nullptr)
        {
            Display::detail::RequestSnapshot(nullptr);
            if (Display::detail::SnapshotTaken()) pixels = frame;
        }
        if (pixels != nullptr) source = 0;
    }
    if (pixels == nullptr)
    {
        const uint16_t* last = Display::detail::LastFramebuffer();
        if (last == nullptr)
        {
            heap_caps_free(frame);
            stream.Fail("no framebuffer: this firmware draws through the panel itself");
            return;
        }
        if (frame != nullptr)
        {
            std::memcpy(frame, last, kPixels * sizeof(uint16_t));
            pixels = frame;
        }
        else
            pixels = last;  // no PSRAM to spare: read it where it is
    }
    const bool runs = RleSize(pixels, kPixels) < kPixels * sizeof(uint16_t);
    const uint8_t header[8] = {'S', 1, static_cast<uint8_t>(Display::WIDTH), static_cast<uint8_t>(Display::WIDTH >> 8),
                               static_cast<uint8_t>(Display::HEIGHT), static_cast<uint8_t>(Display::HEIGHT >> 8),
                               static_cast<uint8_t>(runs ? 1 : 0), source};
    bool sent = stream.Write(header, sizeof(header));
    if (sent && runs) sent = RleEncode(pixels, kPixels, [&](const uint8_t* bytes, std::size_t length) { return stream.Write(bytes, length); });
    else if (sent) sent = stream.Write(pixels, kPixels * sizeof(uint16_t));
    heap_caps_free(frame);
    if (sent) stream.Finish();
}

void Keys(Stream& stream, const uint8_t* args, std::size_t length)
{
    if (length % 3 != 0)
    {
        stream.Fail("KEYS takes steps of three bytes");
        return;
    }
    for (std::size_t at = 0; at < length; at += 3)
    {
        if (g_cancel.load(std::memory_order_relaxed)) break;
        const int key = args[at];
        const bool down = args[at + 1] != 0;
        if (key == kHomeKey)
        {
            g_homeDown.store(down);
            System::detail::InjectHome(down);
        }
        else if (key >= 1 && key <= 56)
            Keyboard::detail::InjectKey(key, down);
        else
        {
            stream.Fail("no such key (1..56, 57 HOME)");
            return;
        }
        if (args[at + 2] != 0) vTaskDelay(pdMS_TO_TICKS(args[at + 2] * 10u));
    }
    StreamSink sink{stream};
    Writer json(sink);
    uint8_t held[56];
    const std::size_t count = Keyboard::detail::InjectedKeys(held, sizeof(held));
    json.Object().Key("down").Array();
    for (std::size_t index = 0; index < count; ++index) json.Uint(held[index]);
    if (g_homeDown.load()) json.Uint(kHomeKey);
    json.End(']').End();
    if (json.Ok()) stream.Finish();
}

// ---- COREDUMP

const esp_partition_t* CoredumpPartition()
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
}

// The dump's length from its first word (ESP-IDF's flash layout: the whole length first); 0: none.
uint32_t CoredumpBytes(FlashReader& flash, const esp_partition_t& partition)
{
    uint8_t word[4];
    if (!flash.Read(partition.address, word, sizeof(word))) return 0;
    const uint32_t length = ReadU32(word);
    return length == 0 || length == 0xFFFFFFFF || length > partition.size ? 0 : length;
}

void CoredumpInfo(Stream& stream)
{
    StreamSink sink{stream};
    Writer json(sink);
    const esp_partition_t* partition = CoredumpPartition();
    FlashReader flash;
    const uint32_t bytes = partition != nullptr ? CoredumpBytes(flash, *partition) : 0;
    json.Object().Key("partition");
    if (partition != nullptr) json.Str(partition->label).Key("size").Uint(partition->size); else json.Null().Key("size").Null();
    json.Key("present").Bool(bytes != 0).Key("bytes").Uint(bytes);
    json.Key("enabled").Bool(kCoredumpEnabled);
    json.Key("summary");
#if CARDENZA_DEV_COREDUMP_SUMMARY
    auto* summary = bytes != 0 ? static_cast<esp_core_dump_summary_t*>(heap_caps_malloc(sizeof(esp_core_dump_summary_t), MALLOC_CAP_8BIT)) : nullptr;
    if (summary != nullptr && esp_core_dump_get_summary(summary) == ESP_OK)
    {
        char address[12];
        std::snprintf(address, sizeof(address), "0x%08lx", static_cast<unsigned long>(summary->exc_pc));
        json.Object().Key("task").Str(summary->exc_task, strnlen(summary->exc_task, sizeof(summary->exc_task)));
        json.Key("pc").Str(address).Key("backtrace").Array();
        const uint32_t depth = std::min<uint32_t>(summary->exc_bt_info.depth, 16);
        for (uint32_t index = 0; index < depth; ++index)
        {
            std::snprintf(address, sizeof(address), "0x%08lx", static_cast<unsigned long>(summary->exc_bt_info.bt[index]));
            json.Str(address);
        }
        json.End(']').Key("corrupted").Bool(summary->exc_bt_info.corrupted).End();
    }
    else
        json.Null();
    heap_caps_free(summary);
#else
    json.Null();
#endif
    json.End();
    if (json.Ok()) stream.Finish();
}

void CoredumpRead(Stream& stream)
{
    const esp_partition_t* partition = CoredumpPartition();
    FlashReader flash;
    const uint32_t bytes = partition != nullptr ? CoredumpBytes(flash, *partition) : 0;
    if (bytes == 0)
    {
        stream.Fail("no core dump");
        return;
    }
    uint8_t buffer[256];
    for (uint32_t at = 0; at < bytes;)
    {
        const uint32_t take = std::min<uint32_t>(sizeof(buffer), bytes - at);
        if (!flash.Read(partition->address + at, buffer, take))
        {
            stream.Fail("flash read failed");
            return;
        }
        if (!stream.Write(buffer, take)) return;
        at += take;
    }
    stream.Finish();
}

void CoredumpErase(Stream& stream)
{
    const esp_partition_t* partition = CoredumpPartition();
    if (partition == nullptr)
    {
        stream.Fail("no core dump partition");
        return;
    }
    // Its first sector holds the length: erased, there is no dump (the next crash erases what it writes over).
    if (esp_partition_erase_range(partition, 0, std::min<uint32_t>(partition->size, 4096)) != ESP_OK)
    {
        stream.Fail("erase failed");
        return;
    }
    StreamSink sink{stream};
    Writer json(sink);
    json.Object().Key("erased").Bool(true).End();
    if (json.Ok()) stream.Finish();
}

// ---- files on the card

constexpr std::size_t kPathCapacity = kMaxPath + 16;

struct Put {
    int fd = -1;
    uint32_t size = 0;
    uint32_t written = 0;
    char path[kPathCapacity] = {};
    char part[kPathCapacity + 5] = {};
};
Put* g_put = nullptr;  // a PUT under way (heap, only then)

void AbortPut()
{
    if (g_put == nullptr) return;
    if (g_put->fd >= 0) close(g_put->fd);
    unlink(g_put->part);
    delete g_put;
    g_put = nullptr;
}

// The request's path on the card, or an error answered.
bool Path(Stream& stream, const uint8_t* path, std::size_t length, bool rootAllowed, char* out)
{
    if (!Storage::IsMounted())
    {
        stream.Fail("no card");
        return false;
    }
    if (!CardPath(Storage::kMountPoint, path, length, rootAllowed, out, kPathCapacity))
    {
        stream.Fail("bad path");
        return false;
    }
    return true;
}

void FailErrno(Stream& stream, const char* what)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%s: %s", what, std::strerror(errno));
    stream.Fail(text);
}

void Done(Stream& stream, const char* key, const char* path)
{
    StreamSink sink{stream};
    Writer json(sink);
    const std::size_t root = std::strlen(Storage::kMountPoint) + 1;
    json.Object().Key(key).Str(std::strlen(path) > root ? path + root : "").End();
    if (json.Ok()) stream.Finish();
}

void List(Stream& stream, const uint8_t* args, std::size_t length)
{
    char path[kPathCapacity];
    if (!Path(stream, args, length, true, path)) return;
    DIR* dir = opendir(path);
    if (dir == nullptr)
    {
        FailErrno(stream, "open");
        return;
    }
    StreamSink sink{stream};
    Writer json(sink);
    json.Object().Key("path").Str(reinterpret_cast<const char*>(args), length).Key("entries").Array();
    char full[kPathCapacity + 260];
    while (json.Ok())
    {
        const dirent* entry = readdir(dir);
        if (entry == nullptr) break;
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        std::snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        struct stat status = {};
        const bool known = stat(full, &status) == 0;
        json.Object().Key("name").Str(entry->d_name).Key("dir").Bool(known ? S_ISDIR(status.st_mode) : entry->d_type == DT_DIR);
        json.Key("bytes");
        if (known && !S_ISDIR(status.st_mode)) json.Uint(static_cast<unsigned long long>(status.st_size)); else json.Null();
        json.Key("modified");
        if (known) json.Int(static_cast<long long>(status.st_mtime)); else json.Null();
        json.End();
    }
    closedir(dir);
    json.End(']').End();
    if (json.Ok()) stream.Finish();
}

void Get(Stream& stream, const uint8_t* args, std::size_t length)
{
    if (length < 4)
    {
        stream.Fail("GET takes an offset and a path");
        return;
    }
    char path[kPathCapacity];
    if (!Path(stream, args + 4, length - 4, false, path)) return;
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        FailErrno(stream, "open");
        return;
    }
    struct stat status = {};
    const uint32_t offset = ReadU32(args);
    if (fstat(fd, &status) != 0 || S_ISDIR(status.st_mode) || offset > static_cast<uint32_t>(status.st_size) ||
        (offset != 0 && lseek(fd, offset, SEEK_SET) < 0))
    {
        close(fd);
        stream.Fail("not a file, or the offset is past its end");
        return;
    }
    uint8_t buffer[512];
    WriteU32(buffer, static_cast<uint32_t>(status.st_size));
    bool ok = stream.Write(buffer, 4);
    while (ok)
    {
        const ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got < 0)
        {
            FailErrno(stream, "read");
            close(fd);
            Storage::CheckAfterIoError();
            return;
        }
        if (got == 0) break;
        ok = stream.Write(buffer, static_cast<std::size_t>(got));
    }
    close(fd);
    if (ok) stream.Finish();
}

void PutBegin(Stream& stream, const uint8_t* args, std::size_t length)
{
    AbortPut();
    if (length < 4)
    {
        stream.Fail("PUT_BEGIN takes a size and a path");
        return;
    }
    auto* put = new (std::nothrow) Put;
    if (put == nullptr)
    {
        stream.Fail("no memory");
        return;
    }
    if (!Path(stream, args + 4, length - 4, false, put->path))
    {
        delete put;
        return;
    }
    put->size = ReadU32(args);
    std::snprintf(put->part, sizeof(put->part), "%s.part", put->path);
    put->fd = open(put->part, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (put->fd < 0)
    {
        FailErrno(stream, "create");
        delete put;
        return;
    }
    g_put = put;
    StreamSink sink{stream};
    Writer json(sink);
    json.Object().Key("path").Str(reinterpret_cast<const char*>(args + 4), length - 4).Key("bytes").Uint(put->size).End();
    if (json.Ok()) stream.Finish();
}

void PutData(Stream& stream, const uint8_t* args, std::size_t length)
{
    if (g_put == nullptr)
    {
        stream.Fail("no PUT_BEGIN");
        return;
    }
    if (length < 4)
    {
        stream.Fail("PUT_DATA takes an offset and bytes");
        return;
    }
    const uint32_t offset = ReadU32(args);
    const std::size_t bytes = length - 4;
    if (offset != g_put->written)
    {
        char text[48];
        std::snprintf(text, sizeof(text), "out of order: expected %lu", static_cast<unsigned long>(g_put->written));
        stream.Fail(text);
        return;
    }
    if (g_put->written + bytes > g_put->size)
    {
        AbortPut();
        stream.Fail("more than the size given");
        return;
    }
    if (bytes != 0 && write(g_put->fd, args + 4, bytes) != static_cast<ssize_t>(bytes))
    {
        FailErrno(stream, "write");
        AbortPut();
        Storage::CheckAfterIoError();
        return;
    }
    g_put->written += static_cast<uint32_t>(bytes);
    uint8_t ack[4];
    WriteU32(ack, g_put->written);
    if (stream.Write(ack, sizeof(ack))) stream.Finish();
}

void PutEnd(Stream& stream)
{
    if (g_put == nullptr)
    {
        stream.Fail("no PUT_BEGIN");
        return;
    }
    if (g_put->written != g_put->size)
    {
        char text[48];
        std::snprintf(text, sizeof(text), "incomplete: %lu of %lu", static_cast<unsigned long>(g_put->written), static_cast<unsigned long>(g_put->size));
        stream.Fail(text);
        return;
    }
    const bool flushed = fsync(g_put->fd) == 0;
    const bool closed = close(g_put->fd) == 0;
    g_put->fd = -1;
    if (!flushed || !closed)
    {
        FailErrno(stream, "write");
        AbortPut();
        Storage::CheckAfterIoError();
        return;
    }
    struct stat status = {};
    if (stat(g_put->path, &status) == 0)
    {
        if (S_ISDIR(status.st_mode) || unlink(g_put->path) != 0)
        {
            FailErrno(stream, "replace");
            AbortPut();
            return;
        }
    }
    if (rename(g_put->part, g_put->path) != 0)
    {
        FailErrno(stream, "rename");
        AbortPut();
        return;
    }
    StreamSink sink{stream};
    Writer json(sink);
    const std::size_t root = std::strlen(Storage::kMountPoint) + 1;
    json.Object().Key("path").Str(g_put->path + root).Key("bytes").Uint(g_put->written).End();
    delete g_put;
    g_put = nullptr;
    if (json.Ok()) stream.Finish();
}

void Remove(Stream& stream, const uint8_t* args, std::size_t length)
{
    char path[kPathCapacity];
    if (!Path(stream, args, length, false, path)) return;
    struct stat status = {};
    if (stat(path, &status) != 0)
    {
        FailErrno(stream, "remove");
        return;
    }
    if ((S_ISDIR(status.st_mode) ? rmdir(path) : unlink(path)) != 0)
    {
        FailErrno(stream, "remove");
        return;
    }
    Done(stream, "removed", path);
}

void MakeDirectory(Stream& stream, const uint8_t* args, std::size_t length)
{
    char path[kPathCapacity];
    if (!Path(stream, args, length, false, path)) return;
    if (mkdir(path, 0777) != 0)
    {
        FailErrno(stream, "mkdir");
        return;
    }
    Done(stream, "made", path);
}

void Move(Stream& stream, const uint8_t* args, std::size_t length)
{
    const auto* split = static_cast<const uint8_t*>(std::memchr(args, 0, length));
    if (split == nullptr)
    {
        stream.Fail("MV takes from, 0, to");
        return;
    }
    char from[kPathCapacity], to[kPathCapacity];
    const std::size_t fromLength = static_cast<std::size_t>(split - args);
    if (!Path(stream, args, fromLength, false, from)) return;
    if (!Path(stream, split + 1, length - fromLength - 1, false, to)) return;
    if (rename(from, to) != 0)
    {
        FailErrno(stream, "move");
        return;
    }
    StreamSink sink{stream};
    Writer json(sink);
    const std::size_t root = std::strlen(Storage::kMountPoint) + 1;
    json.Object().Key("moved").Str(from + root).Key("to").Str(to + root).End();
    if (json.Ok()) stream.Finish();
}

// ---- the worker

void Hello(Stream& stream)
{
    StreamSink sink{stream};
    Writer json(sink);
    json.Object().Key("protocol").Int(kProtocolVersion).Key("max_args").Uint(kMaxArgs).End();
    if (json.Ok()) stream.Finish();
}

void Handle(const Request& request, const uint8_t* args, std::size_t length)
{
    Stream stream(request.id, request.sysex);
    switch (request.op)
    {
        case kHello: Hello(stream); break;
        case kInfo: Info(stream); break;
        case kPartitions: Partitions(stream); break;
        case kMemory: Memory(stream, args, length); break;
        case kStorage: StorageAnswer(stream); break;
        case kStatus: Status(stream); break;
        case kI2c: I2c(stream); break;
        case kScreen: Screen(stream); break;
        case kKeys: Keys(stream, args, length); break;
        case kCoredumpInfo: CoredumpInfo(stream); break;
        case kCoredumpRead: CoredumpRead(stream); break;
        case kCoredumpErase: CoredumpErase(stream); break;
        case kLs: List(stream, args, length); break;
        case kGet: Get(stream, args, length); break;
        case kPutBegin: PutBegin(stream, args, length); break;
        case kPutData: PutData(stream, args, length); break;
        case kPutEnd: PutEnd(stream); break;
        case kPutAbort:
        {
            AbortPut();
            StreamSink sink{stream};
            Writer json(sink);
            json.Object().Key("aborted").Bool(true).End();
            if (json.Ok()) stream.Finish();
            break;
        }
        case kRm: Remove(stream, args, length); break;
        case kMkdir: MakeDirectory(stream, args, length); break;
        case kMv: Move(stream, args, length); break;
        default: stream.Fail("unknown request"); break;
    }
}

// What a worker holds goes when it does: keys, a PUT under way, ARG's buffer.
void LetGo(uint8_t*& argBuffer)
{
    ReleaseKeys();
    AbortPut();
    std::free(argBuffer);
    argBuffer = nullptr;
}

void Worker(void*)
{
    uint8_t* argBuffer = nullptr;  // ARG's, made by the first ARG
    std::size_t argLength = 0;
    uint8_t argId = 0;
    for (;;)
    {
        Request request;
        if (xQueueReceive(g_queue, &request, pdMS_TO_TICKS(kIdleMs)) != pdTRUE)
        {
            LetGo(argBuffer);
            argLength = 0;
            g_running.store(false);
            // A request queued just now: this worker keeps it, unless the USB task has started another already.
            if (uxQueueMessagesWaiting(g_queue) == 0 || g_running.exchange(true)) break;
            continue;
        }
        if (g_cancel.exchange(false))
        {
            // CANCEL came before this request was taken: it goes, with what is queued behind it.
            while (xQueueReceive(g_queue, &request, 0) == pdTRUE) {}
            argLength = 0;
            continue;
        }
        if (request.op == kArg)
        {
            if (argId != request.id) argLength = 0;
            argId = request.id;
            if (argBuffer == nullptr) argBuffer = static_cast<uint8_t*>(std::malloc(kArgBuffer));
            if (argBuffer != nullptr && argLength + request.length <= kArgBuffer)
            {
                std::memcpy(argBuffer + argLength, request.args, request.length);
                argLength += request.length;
            }
            else
                argLength = kArgBuffer + 1;  // too long: the request it is for fails
            continue;
        }
        if (argLength != 0 && argId == request.id)
        {
            const bool fits = argBuffer != nullptr && argLength + request.length <= kArgBuffer;
            if (fits)
            {
                std::memcpy(argBuffer + argLength, request.args, request.length);
                Handle(request, argBuffer, argLength + request.length);
            }
            else
            {
                Stream stream(request.id, request.sysex);
                stream.Fail("arguments too long");
            }
            argLength = 0;
        }
        else
        {
            argLength = 0;
            Handle(request, request.args, request.length);
        }
        if (g_cancel.exchange(false))
            while (xQueueReceive(g_queue, &request, 0) == pdTRUE) {}
    }
    vTaskDelete(nullptr);
}

}  // namespace

void DevRequest(const uint8_t* data, std::size_t length, bool sysex)
{
    if (length < 2) return;
    const uint8_t id = data[0] & 0x7F;
    const uint8_t op = data[1] & 0x7F;
    if (op == kCancel)
    {
        ReleaseKeys();
        if (g_running.load()) g_cancel.store(true);
        return;
    }
    Request request;
    request.id = id;
    request.op = op;
    request.sysex = sysex;
    if (sysex)
        request.length = static_cast<uint8_t>(Unpack7(data + 2, length - 2, request.args, kMaxArgs));
    else
    {
        // A report is always 63 bytes: its args' length comes first.
        const std::size_t given = length >= 3 ? data[2] : 0;
        request.length = static_cast<uint8_t>(std::min({given, length >= 3 ? length - 3 : 0, kMaxArgs}));
        std::memcpy(request.args, data + 3, request.length);
    }
    if (g_queue == nullptr) g_queue = xQueueCreate(kQueueDepth, sizeof(Request));
    if (g_queue == nullptr)
    {
        AnswerNow(id, sysex, "no memory");
        return;
    }
    if (xQueueSend(g_queue, &request, 0) != pdTRUE)
    {
        AnswerNow(id, sysex, "busy");
        return;
    }
    if (!g_running.exchange(true) &&
        xTaskCreatePinnedToCore(Worker, "usbdev", kStackBytes, nullptr, 1, nullptr, tskNO_AFFINITY) != pdPASS)
    {
        g_running.store(false);
        xQueueReset(g_queue);
        AnswerNow(id, sysex, "no memory");
    }
}

void DevHostGone()
{
    ReleaseKeys();
    if (g_running.load()) g_cancel.store(true);
}

#endif  // CONFIG_CARDENZA_HAL_USB_DEV

}  // namespace CardenzaHAL::USB::detail
