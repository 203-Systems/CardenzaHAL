#include "usb/usb.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>

#include <esp_attr.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_private/usb_phy.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_vfs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/rtc_cntl_struct.h>

#include "system/system.h"
#include "tusb.h"
#include "device/usbd_pvt.h"

namespace CardenzaHAL::USB {

namespace {

constexpr char kTag[] = "usb";
constexpr char kConsolePath[] = "/dev/cdzusb";

// Endpoints: four IN endpoints besides EP0 is all the ESP32-S3 has.
constexpr uint8_t kEpMidi = 0x01;
constexpr uint8_t kEpAudioOut = 0x02;   // the host's playback; its feedback goes back on 0x82
constexpr uint8_t kEpAudioIn = 0x83;    // the host's recording
constexpr uint8_t kEpHid = 0x04;

enum : uint8_t { kStringLanguage, kStringManufacturer, kStringProduct, kStringSerial, kStringAudio, kStringMidi, kStringHid, kStringCount };
enum : uint8_t { kReportKeyboard = 1, kReportConsumer = 3 };  // as in Matrix OS

Config g_config;
std::atomic<bool> g_started{false};
usb_phy_handle_t g_phy = nullptr;
char g_serial[13] = {};

// Descriptors, built by Init from what the configuration asks for.
uint8_t g_configuration[TUD_CONFIG_DESC_LEN + CARDENZA_USB_AUDIO_DESC_LEN + 8 + TUD_MIDI_DESC_LEN + TUD_HID_INOUT_DESC_LEN];
uint16_t g_configurationLength = 0;
uint8_t g_hidReport[256];
uint16_t g_hidReportLength = 0;
tusb_desc_device_t g_device;
uint16_t g_string[32];

// Audio state, written from the USB task.
std::atomic<bool> g_playing{false};    // the host plays to the device
std::atomic<bool> g_recording{false};  // the host records from the device
std::atomic<bool> g_hostMute{false};
std::atomic<int16_t> g_hostVolume{0};
std::atomic<uint32_t> g_packetsFromHost{0}, g_bytesFromHost{0};
std::atomic<uint32_t> g_packetsToHost{0}, g_bytesToHost{0}, g_emptyPacketsToHost{0};
std::atomic<int64_t> g_lastFromHostUs{0};
std::atomic<bool> g_windowsHost{false};  // it asked for the Microsoft OS string (feedback format)
std::atomic<uint32_t> g_playbackRestarts{0};
constexpr int16_t kVolumeMin = -90 * 256;
constexpr int16_t kVolumeStep = 256;

// The system channel: commands in, answers and console text out. Reports leave one at a time; an
// answer goes back the way its command came (HID report or SysEx).
std::atomic<ReportHandler> g_appHandler{nullptr};
std::atomic<bool> g_appSysex{false};  // the last app report came as SysEx: answers go back that way
// MIDI packets come from two tasks (the firmware's MidiWrite, the USB task's SysEx answers and
// console): one writer at a time, so a SysEx reaches the host whole.
SemaphoreHandle_t g_midiTx = nullptr;
struct MidiTxLock
{
    MidiTxLock() { if (g_midiTx != nullptr) xSemaphoreTake(g_midiTx, portMAX_DELAY); }
    ~MidiTxLock() { if (g_midiTx != nullptr) xSemaphoreGive(g_midiTx); }
};
std::atomic<bool> g_consoleOn{false};
std::atomic<bool> g_consoleSysex{false};  // the console goes out as SysEx (the command came that way)
std::atomic<bool> g_hostTalked{false};    // a system command arrived (Config::fallbackSeconds)
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
constexpr std::size_t kConsoleRing = 1024;
char g_console[kConsoleRing];
std::size_t g_consoleHead = 0, g_consoleCount = 0;
struct PendingReport {
    uint8_t id;
    uint8_t data[kReportSize];
};
constexpr std::size_t kPendingReports = 4;
PendingReport g_pending[kPendingReports];
std::size_t g_pendingHead = 0, g_pendingCount = 0;
std::atomic<int> g_restart{0};  // a command asked for it; done from the USB task after the answer
// Set before the fallback restart (Config::fallbackSeconds): the next start keeps USB-Serial-JTAG.
constexpr uint32_t kSkipUsbOnce = 0x55534246;
RTC_NOINIT_ATTR uint32_t g_skipUsb;

// MIDI from the host, minus the system SysEx the HAL answers itself. Only the USB task writes it.
constexpr std::size_t kMidiQueue = 64;
uint8_t g_midi[kMidiQueue][4];
std::atomic<std::size_t> g_midiHead{0}, g_midiTail{0};
// A system SysEx being collected (F0 23 ... F7): what follows F0 23.
uint8_t g_sysex[kReportSize];
std::size_t g_sysexLength = 0;
bool g_sysexOurs = false, g_sysexActive = false;

void StoreConsole(const char* data, std::size_t length)
{
    portENTER_CRITICAL_SAFE(&g_lock);
    for (std::size_t index = 0; index < length; ++index)
    {
        g_console[(g_consoleHead + g_consoleCount) % kConsoleRing] = data[index];
        if (g_consoleCount < kConsoleRing) ++g_consoleCount;
        else g_consoleHead = (g_consoleHead + 1) % kConsoleRing;  // the oldest text goes
    }
    portEXIT_CRITICAL_SAFE(&g_lock);
}

bool QueueReport(uint8_t id, const uint8_t* data, std::size_t length)
{
    bool queued = false;
    portENTER_CRITICAL_SAFE(&g_lock);
    if (g_pendingCount < kPendingReports)
    {
        PendingReport& report = g_pending[(g_pendingHead + g_pendingCount) % kPendingReports];
        report.id = id;
        std::memset(report.data, 0, kReportSize);
        std::memcpy(report.data, data, std::min(length, kReportSize));
        ++g_pendingCount;
        queued = true;
    }
    portEXIT_CRITICAL_SAFE(&g_lock);
    return queued;
}

// The next report: a queued one first, then console text when the host asked for it.
bool NextReport(PendingReport& out)
{
    bool any = false;
    portENTER_CRITICAL_SAFE(&g_lock);
    if (g_pendingCount > 0)
    {
        out = g_pending[g_pendingHead];
        g_pendingHead = (g_pendingHead + 1) % kPendingReports;
        --g_pendingCount;
        any = true;
    }
    else if (g_consoleOn.load(std::memory_order_relaxed) && !g_consoleSysex.load(std::memory_order_relaxed) && g_consoleCount > 0)
    {
        const std::size_t length = std::min<std::size_t>(g_consoleCount, kReportSize - 2);
        out.id = kReportSystem;
        std::memset(out.data, 0, kReportSize);
        out.data[0] = kCommandConsole | 0x80;
        out.data[1] = static_cast<uint8_t>(length);
        for (std::size_t index = 0; index < length; ++index) out.data[2 + index] = static_cast<uint8_t>(g_console[(g_consoleHead + index) % kConsoleRing]);
        g_consoleHead = (g_consoleHead + length) % kConsoleRing;
        g_consoleCount -= length;
        any = true;
    }
    portEXIT_CRITICAL_SAFE(&g_lock);
    return any;
}

void PumpReports()
{
    if (!tud_hid_ready()) return;
    PendingReport report;
    if (NextReport(report)) tud_hid_report(report.id, report.data, kReportSize);
}

// An answer as SysEx: F0 24 <bytes, 7-bit> F7, in USB-MIDI packets of up to three bytes.
void SendSysex(const uint8_t* data, std::size_t length)
{
    MidiTxLock lock;
    uint8_t message[kReportSize + 3];
    std::size_t size = 0;
    message[size++] = 0xF0;
    message[size++] = 0x24;
    for (std::size_t index = 0; index < length && size < sizeof(message) - 1; ++index) message[size++] = data[index] & 0x7F;
    message[size++] = 0xF7;
    for (std::size_t at = 0; at < size; at += 3)
    {
        const std::size_t left = size - at;
        uint8_t packet[4] = {0x04, 0, 0, 0};  // SysEx starts or continues
        if (left <= 3) packet[0] = static_cast<uint8_t>(0x04 + left);  // ends with 1, 2 or 3 bytes: 0x5, 0x6, 0x7
        for (std::size_t index = 0; index < 3 && index < left; ++index) packet[1 + index] = message[at + index];
        tud_midi_packet_write(packet);
    }
}

void Answer(bool sysex, const uint8_t* data, std::size_t length)
{
    if (sysex) SendSysex(data, length);
    else QueueReport(kReportSystem, data, length);
}

void HandleCommand(const uint8_t* data, std::size_t length, bool sysex)
{
    if (length == 0) return;
    g_hostTalked.store(true, std::memory_order_relaxed);
    const uint8_t command = data[0];
    switch (command)
    {
        case kCommandIdentify:
        {
            uint8_t answer[kReportSize] = {static_cast<uint8_t>(command | 0x80), kProtocolVersion};
            std::strncpy(reinterpret_cast<char*>(answer + 2), g_config.product, kReportSize - 3);
            Answer(sysex, answer, 2 + std::strlen(reinterpret_cast<char*>(answer + 2)));
            return;
        }
        case kCommandBootloader:
        case kCommandReboot:
        case kCommandDownload:
        {
            const uint8_t answer[1] = {static_cast<uint8_t>(command | 0x80)};
            Answer(sysex, answer, sizeof(answer));
            g_restart.store(command, std::memory_order_relaxed);
            return;
        }
        case kCommandConsole:
            g_consoleSysex.store(sysex, std::memory_order_relaxed);
            g_consoleOn.store(length > 1 && data[1] != 0, std::memory_order_relaxed);
            return;
        case kCommandApp:
            if (!sysex) return;  // over HID the firmware has its own report ID
            g_appSysex.store(true, std::memory_order_relaxed);
            if (ReportHandler handler = g_appHandler.load(std::memory_order_relaxed)) handler(data + 1, length - 1);
            return;
        default:
            return;
    }
}

// Console text as SysEx (F0 24 C3 <text> F7), when the host asked for it that way.
void PumpSysexConsole()
{
    if (!g_consoleOn.load(std::memory_order_relaxed) || !g_consoleSysex.load(std::memory_order_relaxed)) return;
    uint8_t text[kReportSize];
    std::size_t length = 0;
    portENTER_CRITICAL_SAFE(&g_lock);
    length = std::min<std::size_t>(g_consoleCount, 48);
    for (std::size_t index = 0; index < length; ++index) text[1 + index] = static_cast<uint8_t>(g_console[(g_consoleHead + index) % kConsoleRing]);
    g_consoleHead = (g_consoleHead + length) % kConsoleRing;
    g_consoleCount -= length;
    portEXIT_CRITICAL_SAFE(&g_lock);
    if (length == 0) return;
    text[0] = kCommandConsole | 0x80;
    SendSysex(text, length + 1);
}

// Reads what the host sent over MIDI: system SysEx is answered here, the rest waits for MidiRead.
void PumpMidi()
{
    uint8_t packet[4];
    while (tud_midi_available() && tud_midi_packet_read(packet))
    {
        const uint8_t code = packet[0] & 0x0F;
        const bool sysexPacket = code == 0x04 || code == 0x05 || code == 0x06 || code == 0x07;
        const bool startsSysex = code == 0x04 && packet[1] == 0xF0;
        if (startsSysex || (sysexPacket && g_sysexActive))
        {
            if (startsSysex)
            {
                g_sysexActive = true;
                g_sysexOurs = packet[2] == 0x23;
                g_sysexLength = 0;
                // F0 23 <first byte of the command>
                if (g_sysexOurs) g_sysex[g_sysexLength++] = packet[3];
            }
            else if (g_sysexOurs)
            {
                const std::size_t count = code == 0x05 ? 1 : (code == 0x06 ? 2 : 3);
                for (std::size_t index = 0; index < count; ++index)
                {
                    if (packet[1 + index] == 0xF7) break;
                    if (g_sysexLength < kReportSize) g_sysex[g_sysexLength++] = packet[1 + index];
                }
            }
            if (code != 0x04)
            {
                if (g_sysexOurs) HandleCommand(g_sysex, g_sysexLength, true);
                g_sysexActive = false;
            }
            // Someone else's SysEx goes on to the firmware.
            if (g_sysexOurs) continue;
        }
        const std::size_t tail = g_midiTail.load(std::memory_order_relaxed);
        const std::size_t next = (tail + 1) % kMidiQueue;
        if (next == g_midiHead.load(std::memory_order_acquire)) continue;  // full: the newest goes
        std::memcpy(g_midi[tail], packet, 4);
        g_midiTail.store(next, std::memory_order_release);
    }
}

// The console as a file: stdout and stderr are reopened on it.
ssize_t ConsoleWrite(int, const void* data, size_t size)
{
    StoreConsole(static_cast<const char*>(data), size);
    return static_cast<ssize_t>(size);
}
int ConsoleOpen(const char*, int, int) { return 0; }
int ConsoleClose(int) { return 0; }
int ConsoleFstat(int, struct stat* status)
{
    std::memset(status, 0, sizeof(*status));
    status->st_mode = S_IFCHR;
    return 0;
}

void RedirectConsole()
{
    esp_vfs_t vfs = {};
    vfs.flags = ESP_VFS_FLAG_DEFAULT;
    vfs.write = &ConsoleWrite;
    vfs.open = &ConsoleOpen;
    vfs.close = &ConsoleClose;
    vfs.fstat = &ConsoleFstat;
    if (esp_vfs_register(kConsolePath, &vfs, nullptr) != ESP_OK) return;
    char path[24];
    std::snprintf(path, sizeof(path), "%s/0", kConsolePath);
    if (std::freopen(path, "w", stdout) != nullptr) setvbuf(stdout, nullptr, _IOLBF, 0);
    if (std::freopen(path, "w", stderr) != nullptr) setvbuf(stderr, nullptr, _IONBF, 0);
}

void BuildDescriptors()
{
    uint8_t interfaces = 0;
    uint8_t* at = g_configuration + TUD_CONFIG_DESC_LEN;
    const auto append = [&](const uint8_t* block, std::size_t length) {
        std::memcpy(at, block, length);
        at += length;
    };
    if (g_config.audio)
    {
        const uint8_t itf = interfaces;
        const uint8_t block[] = {CARDENZA_USB_AUDIO_DESCRIPTOR(itf, kStringAudio, kEpAudioOut, (uint8_t)(0x80 | kEpAudioOut), kEpAudioIn)};
        append(block, sizeof(block));
        interfaces += 3;
    }
    if (g_config.midi)
    {
        const uint8_t itf = interfaces;
        // Its own interface association: next to a UAC2 function macOS does not take the MIDI interfaces
        // without one.
        const uint8_t association[] = {8, TUSB_DESC_INTERFACE_ASSOCIATION, itf, 2, TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_CONTROL, AUDIO_FUNC_PROTOCOL_CODE_UNDEF, 0};
        append(association, sizeof(association));
        const uint8_t block[] = {TUD_MIDI_DESCRIPTOR(itf, kStringMidi, kEpMidi, (uint8_t)(0x80 | kEpMidi), 64)};
        append(block, sizeof(block));
        interfaces += 2;
    }
    // HID: keyboard and consumer keys when asked for, the system and app channels always.
    g_hidReportLength = 0;
    const auto report = [&](const uint8_t* block, std::size_t length) {
        std::memcpy(g_hidReport + g_hidReportLength, block, length);
        g_hidReportLength = static_cast<uint16_t>(g_hidReportLength + length);
    };
    if (g_config.keyboard)
    {
        const uint8_t block[] = {TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(kReportKeyboard))};
        report(block, sizeof(block));
    }
    if (g_config.consumer)
    {
        const uint8_t block[] = {TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(kReportConsumer))};
        report(block, sizeof(block));
    }
    for (const uint8_t id : {kReportSystem, kReportApp})
    {
        const uint8_t block[] = {TUD_HID_REPORT_DESC_GENERIC_INOUT(kReportSize, HID_REPORT_ID(id))};
        report(block, sizeof(block));
    }
    {
        const uint8_t itf = interfaces;
        const uint8_t block[] = {TUD_HID_INOUT_DESCRIPTOR(itf, kStringHid, HID_ITF_PROTOCOL_NONE, g_hidReportLength, kEpHid, (uint8_t)(0x80 | kEpHid), 64, 1)};
        append(block, sizeof(block));
        interfaces += 1;
    }
    g_configurationLength = static_cast<uint16_t>(at - g_configuration);
    const uint8_t header[] = {TUD_CONFIG_DESCRIPTOR(1, interfaces, 0, g_configurationLength, 0x00, 500)};
    std::memcpy(g_configuration, header, sizeof(header));

    g_device = {};
    g_device.bLength = sizeof(tusb_desc_device_t);
    g_device.bDescriptorType = TUSB_DESC_DEVICE;
    g_device.bcdUSB = 0x0200;
    // Interface association descriptors (the audio function has one).
    g_device.bDeviceClass = TUSB_CLASS_MISC;
    g_device.bDeviceSubClass = MISC_SUBCLASS_COMMON;
    g_device.bDeviceProtocol = MISC_PROTOCOL_IAD;
    g_device.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE;
    g_device.idVendor = g_config.vendorId;
    g_device.idProduct = g_config.productId;
    g_device.bcdDevice = g_config.deviceVersion;
    g_device.iManufacturer = kStringManufacturer;
    g_device.iProduct = kStringProduct;
    g_device.iSerialNumber = kStringSerial;
    g_device.bNumConfigurations = 1;
}

// TinyUSB's DWC2 driver retries an isochronous IN transfer that missed its frame, but not an OUT one:
// re-armed a frame late (the USB interrupt held up, by a flash write for one), the host's playback
// endpoint waits for a frame parity that has passed and never receives again. While the host plays
// and nothing has arrived for 20 frames, it is activated afresh and the next packet scheduled.
constexpr int64_t kPlaybackStallUs = 20000;
const uint8_t kPlaybackEndpoint[] = {7, TUSB_DESC_ENDPOINT, kEpAudioOut,
                                     (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS | TUSB_ISO_EP_ATT_DATA),
                                     U16_TO_U8S_LE(CARDENZA_USB_AUDIO_EP_SIZE), 1};

void WatchPlayback()
{
    if (!g_config.audio || !g_playing.load(std::memory_order_relaxed)) return;
    const int64_t now = esp_timer_get_time();
    if (now - g_lastFromHostUs.load(std::memory_order_relaxed) < kPlaybackStallUs) return;
    tu_fifo_t* fifo = tud_audio_get_ep_out_ff();
    if (fifo == nullptr) return;
    g_lastFromHostUs.store(now, std::memory_order_relaxed);  // one attempt per stall time
    const auto* endpoint = reinterpret_cast<const tusb_desc_endpoint_t*>(kPlaybackEndpoint);
    if (usbd_edpt_iso_activate(0, endpoint) && usbd_edpt_xfer_fifo(0, kEpAudioOut, fifo, CARDENZA_USB_AUDIO_EP_SIZE, false))
        g_playbackRestarts.fetch_add(1, std::memory_order_relaxed);
}

[[noreturn]] void Restart()
{
    if (g_started.load())
    {
        tud_disconnect();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    // USB-Serial-JTAG gets the PHY back, for whatever starts next (the Launcher's console, esptool).
    RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 0;
    esp_restart();
}

void UsbTask(void*)
{
    const TickType_t started = xTaskGetTickCount();
    for (;;)
    {
        if (g_config.fallbackSeconds != 0 && !g_hostTalked.load(std::memory_order_relaxed) &&
            xTaskGetTickCount() - started > pdMS_TO_TICKS(1000u * g_config.fallbackSeconds))
        {
            ESP_LOGE(kTag, "no host command in %u s: restarting without USB", g_config.fallbackSeconds);
            g_skipUsb = kSkipUsbOnce;
            Restart();
        }
        tud_task_ext(1, false);  // at most a millisecond, so the reports keep moving
        if (g_config.midi)
        {
            PumpMidi();
            PumpSysexConsole();
        }
        PumpReports();
        WatchPlayback();
        const int restart = g_restart.exchange(0, std::memory_order_relaxed);
        if (restart != 0)
        {
            // Let the answer go out first (Matrix OS waits 20 ms too).
            for (int wait = 0; wait < 20; ++wait)
            {
                tud_task_ext(1, false);
                PumpReports();
            }
            if (restart == kCommandDownload) RestartIntoDownloadMode();
            if (restart == kCommandBootloader) RestartIntoLauncher();
            Restart();
        }
    }
}

}  // namespace

bool Init(const Config& config)
{
    if (g_started.load()) return false;
    if (g_skipUsb == kSkipUsbOnce)
    {
        g_skipUsb = 0;
        ESP_LOGW(kTag, "the last start found no host: USB stays on USB-Serial-JTAG this time");
        return false;
    }
    // The way back to esptool on a device whose USB firmware does not answer: hold Func at start.
    if (System::GetFuncButton())
    {
        ESP_LOGW(kTag, "Func held: USB stays on USB-Serial-JTAG");
        return false;
    }
    g_config = config;
    if (g_midiTx == nullptr) g_midiTx = xSemaphoreCreateMutex();
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    std::snprintf(g_serial, sizeof(g_serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    BuildDescriptors();

    usb_phy_config_t phy = {};
    phy.controller = USB_PHY_CTRL_OTG;
    phy.target = USB_PHY_TARGET_INT;
    phy.otg_mode = USB_OTG_MODE_DEVICE;
    if (usb_new_phy(&phy, &g_phy) != ESP_OK)
    {
        ESP_LOGE(kTag, "USB PHY failed");
        return false;
    }
    if (g_config.console) RedirectConsole();
    if (!tusb_init())
    {
        ESP_LOGE(kTag, "TinyUSB failed");
        usb_del_phy(g_phy);
        g_phy = nullptr;
        return false;
    }
    if (xTaskCreatePinnedToCore(UsbTask, "usb", 4096, nullptr, 8, nullptr, 0) != pdPASS)
    {
        ESP_LOGE(kTag, "USB task failed");
        return false;
    }
    g_started.store(true);
    ESP_LOGI(kTag, "started %04x:%04x audio=%d midi=%d keyboard=%d consumer=%d", g_config.vendorId, g_config.productId,
             g_config.audio, g_config.midi, g_config.keyboard, g_config.consumer);
    return true;
}

bool Started() { return g_started.load(); }
bool Mounted() { return g_started.load() && tud_mounted() && !tud_suspended(); }

bool MidiRead(uint8_t packet[4])
{
    const std::size_t head = g_midiHead.load(std::memory_order_relaxed);
    if (head == g_midiTail.load(std::memory_order_acquire)) return false;
    std::memcpy(packet, g_midi[head], 4);
    g_midiHead.store((head + 1) % kMidiQueue, std::memory_order_release);
    return true;
}
bool MidiWrite(const uint8_t packet[4])
{
    if (!Mounted() || !g_config.midi) return false;
    MidiTxLock lock;
    return tud_midi_packet_write(packet);
}

std::size_t AudioWrite(const int16_t* frames, std::size_t count)
{
    // Nobody records: the frames go nowhere, as with a cable that is not plugged in.
    if (!g_recording.load(std::memory_order_relaxed)) return count;
    const std::size_t bytes = count * CARDENZA_USB_AUDIO_CHANNELS * CARDENZA_USB_AUDIO_BYTES;
    return tud_audio_write(frames, static_cast<uint16_t>(bytes)) / (CARDENZA_USB_AUDIO_CHANNELS * CARDENZA_USB_AUDIO_BYTES);
}

std::size_t AudioRead(int16_t* frames, std::size_t count)
{
    if (!g_playing.load(std::memory_order_relaxed)) return 0;
    constexpr std::size_t kFrameBytes = CARDENZA_USB_AUDIO_CHANNELS * CARDENZA_USB_AUDIO_BYTES;
    const std::size_t available = tud_audio_available() / kFrameBytes;
    const std::size_t take = std::min(count, available);
    return take == 0 ? 0 : tud_audio_read(frames, static_cast<uint16_t>(take * kFrameBytes)) / kFrameBytes;
}

AudioCounters AudioStats()
{
    AudioCounters counters;
    counters.packetsFromHost = g_packetsFromHost.load(std::memory_order_relaxed);
    counters.bytesFromHost = g_bytesFromHost.load(std::memory_order_relaxed);
    counters.packetsToHost = g_packetsToHost.load(std::memory_order_relaxed);
    counters.bytesToHost = g_bytesToHost.load(std::memory_order_relaxed);
    counters.emptyPacketsToHost = g_emptyPacketsToHost.load(std::memory_order_relaxed);
    counters.playbackRestarts = g_playbackRestarts.load(std::memory_order_relaxed);
    if (g_started.load() && g_config.audio)
    {
        counters.fifoFromHost = tud_audio_available();
        if (tu_fifo_t* fifo = tud_audio_get_ep_in_ff()) counters.fifoToHost = tu_fifo_count(fifo);
    }
    return counters;
}

bool AudioInputActive() { return g_recording.load(std::memory_order_relaxed); }
bool AudioOutputActive() { return g_playing.load(std::memory_order_relaxed); }
bool HostMuted() { return g_hostMute.load(std::memory_order_relaxed); }
int16_t HostVolume() { return g_hostVolume.load(std::memory_order_relaxed); }

bool KeyboardReport(uint8_t modifiers, const uint8_t keys[6])
{
    if (!Mounted() || !g_config.keyboard || !tud_hid_ready()) return false;
    return tud_hid_keyboard_report(kReportKeyboard, modifiers, keys);
}

bool ConsumerReport(uint16_t usage)
{
    if (!Mounted() || !g_config.consumer || !tud_hid_ready()) return false;
    return tud_hid_report(kReportConsumer, &usage, sizeof(usage));
}

void SetAppReportHandler(ReportHandler handler) { g_appHandler.store(handler, std::memory_order_relaxed); }

bool SendAppReport(const uint8_t* data, std::size_t length)
{
    if (!g_started.load()) return false;
    if (!g_appSysex.load(std::memory_order_relaxed)) return QueueReport(kReportApp, data, length);
    if (!Mounted() || !g_config.midi) return false;
    uint8_t message[kReportSize];
    message[0] = kCommandApp;
    length = std::min(length, kReportSize - 1);
    std::memcpy(message + 1, data, length);
    SendSysex(message, length + 1);
    return true;
}

void RestartIntoDownloadMode()
{
    ESP_LOGW(kTag, "restarting into the ROM download mode");
    if (g_started.load())
    {
        tud_disconnect();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    // The PHY goes back to USB-Serial-JTAG (this survives the restart) and the ROM stays in download mode.
    RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 0;
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

void RestartIntoLauncher()
{
    ESP_LOGW(kTag, "restarting into the Launcher");
    Restart();
}

}  // namespace CardenzaHAL::USB

// TinyUSB callbacks (C linkage, one definition each per firmware).
using namespace CardenzaHAL::USB;

extern "C" {

uint8_t const* tud_descriptor_device_cb(void) { return reinterpret_cast<uint8_t const*>(&g_device); }

uint8_t const* tud_descriptor_configuration_cb(uint8_t) { return g_configuration; }

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t)
{
    if (index == 0xEE)
    {
        g_windowsHost.store(true, std::memory_order_relaxed);  // only Windows asks for it
        return nullptr;
    }
    const char* text = nullptr;
    switch (index)
    {
        case kStringLanguage:
            g_string[1] = 0x0409;
            g_string[0] = static_cast<uint16_t>((TUSB_DESC_STRING << 8) | 4);
            return g_string;
        case kStringManufacturer: text = g_config.manufacturer; break;
        case kStringProduct: text = g_config.product; break;
        case kStringSerial: text = g_serial; break;
        case kStringAudio: text = g_config.product; break;
        case kStringMidi: text = g_config.product; break;
        case kStringHid: text = g_config.product; break;
        default: return nullptr;
    }
    std::size_t length = std::min<std::size_t>(std::strlen(text), 31);
    for (std::size_t i = 0; i < length; ++i) g_string[1 + i] = static_cast<uint8_t>(text[i]);
    g_string[0] = static_cast<uint16_t>((TUSB_DESC_STRING << 8) | (2 * length + 2));
    return g_string;
}

uint8_t const* tud_hid_descriptor_report_cb(uint8_t) { return g_hidReport; }

uint16_t tud_hid_get_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t*, uint16_t) { return 0; }

void tud_hid_set_report_cb(uint8_t, uint8_t report_id, hid_report_type_t, uint8_t const* buffer, uint16_t bufsize)
{
    // On the OUT endpoint the report ID comes as the first byte.
    if (report_id == 0 && bufsize > 0)
    {
        report_id = buffer[0];
        ++buffer;
        --bufsize;
    }
    if (report_id == kReportSystem) HandleCommand(buffer, bufsize, false);
    else if (report_id == kReportApp)
    {
        g_appSysex.store(false, std::memory_order_relaxed);
        if (ReportHandler handler = g_appHandler.load(std::memory_order_relaxed)) handler(buffer, bufsize);
    }
}

void tud_hid_report_complete_cb(uint8_t, uint8_t const*, uint16_t) { PumpReports(); }

void tud_umount_cb(void)
{
    g_playing.store(false);
    g_recording.store(false);
}

// Audio: the clock, the host's volume control, and which streams run.
static bool AudioClockRequest(uint8_t rhport, tusb_control_request_t const* request)
{
    const uint8_t control = TU_U16_HIGH(request->wValue);
    if (control == AUDIO20_CS_CTRL_SAM_FREQ)
    {
        if (request->bRequest == AUDIO20_CS_REQ_CUR)
        {
            audio20_control_cur_4_t current = {static_cast<int32_t>(tu_htole32(kSampleRate))};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &current, sizeof(current));
        }
        if (request->bRequest == AUDIO20_CS_REQ_RANGE)
        {
            audio20_control_range_4_n_t(1) range = {};
            range.wNumSubRanges = tu_htole16(1);
            range.subrange[0].bMin = static_cast<int32_t>(kSampleRate);
            range.subrange[0].bMax = static_cast<int32_t>(kSampleRate);
            range.subrange[0].bRes = 0;
            return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &range, sizeof(range));
        }
    }
    else if (control == AUDIO20_CS_CTRL_CLK_VALID && request->bRequest == AUDIO20_CS_REQ_CUR)
    {
        audio20_control_cur_1_t valid = {1};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &valid, sizeof(valid));
    }
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const* request)
{
    const uint8_t entity = TU_U16_HIGH(request->wIndex);
    const uint8_t control = TU_U16_HIGH(request->wValue);
    if (entity == CARDENZA_USB_AUDIO_CLOCK) return AudioClockRequest(rhport, request);
    if (entity != CARDENZA_USB_AUDIO_SPK_FEATURE) return false;
    if (control == AUDIO20_FU_CTRL_MUTE && request->bRequest == AUDIO20_CS_REQ_CUR)
    {
        audio20_control_cur_1_t mute = {static_cast<int8_t>(g_hostMute.load() ? 1 : 0)};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &mute, sizeof(mute));
    }
    if (control == AUDIO20_FU_CTRL_VOLUME)
    {
        if (request->bRequest == AUDIO20_CS_REQ_CUR)
        {
            audio20_control_cur_2_t volume = {static_cast<int16_t>(tu_htole16(g_hostVolume.load()))};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &volume, sizeof(volume));
        }
        if (request->bRequest == AUDIO20_CS_REQ_RANGE)
        {
            audio20_control_range_2_n_t(1) range = {};
            range.wNumSubRanges = tu_htole16(1);
            range.subrange[0].bMin = static_cast<int16_t>(kVolumeMin);
            range.subrange[0].bMax = 0;
            range.subrange[0].bRes = static_cast<int16_t>(kVolumeStep);
            return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &range, sizeof(range));
        }
    }
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t, tusb_control_request_t const* request, uint8_t* buffer)
{
    const uint8_t entity = TU_U16_HIGH(request->wIndex);
    const uint8_t control = TU_U16_HIGH(request->wValue);
    if (request->bRequest != AUDIO20_CS_REQ_CUR) return false;
    if (entity == CARDENZA_USB_AUDIO_CLOCK && control == AUDIO20_CS_CTRL_SAM_FREQ)
    {
        int32_t rate = 0;
        std::memcpy(&rate, buffer, sizeof(rate));
        return static_cast<uint32_t>(rate) == kSampleRate;
    }
    if (entity != CARDENZA_USB_AUDIO_SPK_FEATURE) return false;
    if (control == AUDIO20_FU_CTRL_MUTE)
    {
        g_hostMute.store(buffer[0] != 0);
        return true;
    }
    if (control == AUDIO20_FU_CTRL_VOLUME)
    {
        int16_t volume = 0;
        std::memcpy(&volume, buffer, sizeof(volume));
        g_hostVolume.store(std::clamp<int16_t>(volume, kVolumeMin, 0));
        return true;
    }
    return false;
}

bool tud_audio_set_itf_cb(uint8_t, tusb_control_request_t const* request)
{
    const uint8_t itf = TU_U16_LOW(request->wIndex);
    const uint8_t alt = TU_U16_LOW(request->wValue);
    // The audio function's interfaces are the first three when it is there.
    if (itf == 1)
    {
        g_lastFromHostUs.store(esp_timer_get_time(), std::memory_order_relaxed);
        g_playing.store(alt != 0);
    }
    if (itf == 2)
    {
        if (alt != 0) tud_audio_clear_ep_in_ff();
        g_recording.store(alt != 0);
    }
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t, tusb_control_request_t const* request)
{
    const uint8_t itf = TU_U16_LOW(request->wIndex);
    if (itf == 1) g_playing.store(false);
    if (itf == 2) g_recording.store(false);
    return true;
}

// TinyUSB's audio driver allocates the feedback endpoint's TX FIFO for its 4 bytes: one word. The DWC2
// documentation gives 16 words as the smallest TX FIFO, so the feedback endpoint gets that (the FIFO
// RAM has room: 229 of 256 words in use); everything else is allocated as asked.
bool __real_usbd_edpt_iso_alloc(uint8_t rhport, uint8_t ep_addr, uint16_t largest_packet_size);
bool __wrap_usbd_edpt_iso_alloc(uint8_t rhport, uint8_t ep_addr, uint16_t largest_packet_size)
{
    if (ep_addr == (0x80 | kEpAudioOut) && largest_packet_size < 64) largest_packet_size = 64;
    return __real_usbd_edpt_iso_alloc(rhport, ep_addr, largest_packet_size);
}

// The feedback value. TinyUSB sends UAC2 feedback as 16.16 in 4 bytes at every speed, which is what
// Windows' driver wants; on full speed the USB specification says 10.14 in 3 bytes, and macOS reads it
// that way: 16.16 there means four times the rate, and macOS stops playing after its first 128 ms. A
// host that asks for the Microsoft OS string (index 0xEE, tud_descriptor_string_cb) is taken for
// Windows and gets 16.16; every other host gets 10.14.
bool __real_usbd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes, bool is_isr);
bool __wrap_usbd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes, bool is_isr)
{
    if (ep_addr != (0x80 | kEpAudioOut) || total_bytes != 4 || g_windowsHost.load(std::memory_order_relaxed))
        return __real_usbd_edpt_xfer(rhport, ep_addr, buffer, total_bytes, is_isr);
    // Only one feedback transfer is in flight at a time, so one buffer will do.
    static uint8_t feedback[4] __attribute__((aligned(4)));
    uint32_t value = 0;
    std::memcpy(&value, buffer, sizeof(value));
    value >>= 2;
    feedback[0] = static_cast<uint8_t>(value);
    feedback[1] = static_cast<uint8_t>(value >> 8);
    feedback[2] = static_cast<uint8_t>(value >> 16);
    feedback[3] = 0;
    return __real_usbd_edpt_xfer(rhport, ep_addr, feedback, 3, is_isr);
}

// Every audio packet, in the USB interrupt: counted for AudioStats.
bool tud_audio_rx_done_isr(uint8_t, uint16_t bytes, uint8_t, uint8_t, uint8_t)
{
    g_lastFromHostUs.store(esp_timer_get_time(), std::memory_order_relaxed);
    g_packetsFromHost.fetch_add(1, std::memory_order_relaxed);
    g_bytesFromHost.fetch_add(bytes, std::memory_order_relaxed);
    return true;
}

bool tud_audio_tx_done_isr(uint8_t, uint16_t bytes, uint8_t, uint8_t, uint8_t)
{
    g_packetsToHost.fetch_add(1, std::memory_order_relaxed);
    g_bytesToHost.fetch_add(bytes, std::memory_order_relaxed);
    if (bytes == 0) g_emptyPacketsToHost.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void tud_audio_feedback_params_cb(uint8_t, uint8_t, audio_feedback_params_t* feedback)
{
    // The host's rate follows how full the playback FIFO is: the device's clock leads.
    feedback->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
    feedback->sample_freq = kSampleRate;
}

}  // extern "C"
