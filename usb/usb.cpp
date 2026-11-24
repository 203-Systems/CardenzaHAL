#include "usb/usb.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>

#include <esp_app_desc.h>
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

#include "system/internal/hooks.h"
#include "system/launcher.h"
#include "usb/dev_logic.h"
#include "usb/internal/camera.h"
#include "usb/internal/dev.h"
#include "usb/internal/start.h"
#include "usb/usb_logic.h"
#include "tusb.h"
#include "device/usbd_pvt.h"

namespace CardenzaHAL::USB {

namespace {

constexpr char kTag[] = "usb";
constexpr char kConsolePath[] = "/dev/cdzusb";
constexpr char kBoardName[] = "Cardenza";
constexpr char kManufacturer[] = "203 Systems";

// What the build has (tusb_config.h, from Kconfig).
constexpr Logic::Classes kBuilt =
    Logic::Built(CARDENZA_USB_BUILD_MIDI, CARDENZA_USB_BUILD_AUDIO, CARDENZA_USB_BUILD_AUDIO_FEEDBACK, CARDENZA_USB_BUILD_HID,
                 CARDENZA_USB_BUILD_WEBUSB, CARDENZA_USB_BUILD_SERIAL, CARDENZA_USB_BUILD_DRIVE, CARDENZA_USB_BUILD_CAMERA,
                 CARDENZA_USB_BUILD_NETWORK);

constexpr Logic::Classes Asked(const Config& config)
{
    Logic::Classes classes;
    classes.midi = config.midi;
    classes.audio = config.audio;
    classes.playback = config.audioPlayback;
    classes.feedback = config.audioFeedback;
    classes.hid = config.hid;
    classes.webusb = config.webusb;
    classes.serial = config.serial;
    classes.drive = config.drive;
    classes.camera = config.camera;
    classes.network = config.network;
    return classes;
}

// The classes on by default fit (tusb_config.h stops the build earlier with what to turn off).
static_assert(Logic::Allocate(Logic::Effective(Asked(Config()), kBuilt)).fits,
              "CardenzaHAL USB: the classes on by default need more than the ESP32-S3's 4 IN endpoints (menuconfig: CardenzaHAL > USB)");

enum : uint8_t {
    kStringLanguage,
    kStringManufacturer,
    kStringProduct,
    kStringSerial,
    kStringAudio,
    kStringMidi,
    kStringHid,
    kStringSerialPort,
    kStringWebUsb,
    kStringDrive,
    kStringNetwork,
    kStringNetworkMac,
    kStringCount
};
enum : uint8_t { kReportKeyboard = 1, kReportConsumer = 3 };  // as in Matrix OS

// Device-level vendor requests (the BOS names them): the WebUSB landing page and Microsoft OS 2.0.
constexpr uint8_t kVendorWebUsb = 0x01;
constexpr uint8_t kVendorMicrosoft = 0x02;
constexpr uint16_t kWebUsbGetUrl = 2;
constexpr uint16_t kMsOs20Descriptor = 7;
constexpr std::size_t kMsOs20Length = 0xB2;
constexpr std::size_t kBosLength = TUD_BOS_DESC_LEN + TUD_BOS_WEBUSB_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN;

constexpr std::size_t kConfigurationMax = TUD_CONFIG_DESC_LEN + CARDENZA_USB_AUDIO_DESC_LEN + 8 + TUD_MIDI_DESC_LEN +
                                          TUD_HID_INOUT_DESC_LEN + TUD_CDC_DESC_LEN + 9 + 7 + TUD_MSC_DESC_LEN +
                                          TUD_CDC_NCM_DESC_LEN;

// Everything the computer reads about the device: built together, swapped together.
struct Descriptors {
    tusb_desc_device_t device;
    uint8_t configuration[kConfigurationMax];
    uint16_t configurationLength;
    uint8_t hidReport[384];
    uint16_t hidReportLength;
    uint8_t bos[kBosLength];
    uint8_t msOs20[kMsOs20Length];
    bool webusb;
    bool audioPlayback;
    uint8_t itfAudio;
    uint8_t itfWebUsb;
};

Config g_config;
Logic::Classes g_asked;     // what the firmware asked for (Config, plus the drive and the camera)
Logic::Classes g_classes;   // what the device presents
Logic::Endpoints g_ep;
Descriptors g_desc;
Descriptors g_next;         // the next ones, built beside (static: about 1.5 KB)
std::atomic<bool> g_started{false};
std::atomic<bool> g_auto{false};          // started by CardenzaHAL::Init; the firmware's Init takes it over
std::atomic<int64_t> g_connectAtUs{0};    // the HAL's own start connects then (0: connected)
bool g_skippedThisBoot = false;
SemaphoreHandle_t g_configLock = nullptr;  // Init, Reconfigure, SetClasses and the delayed connect
usb_phy_handle_t g_phy = nullptr;
char g_serial[18] = {};
uint16_t g_string[48];

// Audio state, written from the USB task.
std::atomic<bool> g_playing{false};    // the host plays to the device
std::atomic<bool> g_recording{false};  // the host records from the device
std::atomic<bool> g_hostMute{false};
std::atomic<int16_t> g_hostVolume{0};
std::atomic<uint32_t> g_packetsFromHost{0}, g_bytesFromHost{0};
std::atomic<uint32_t> g_packetsToHost{0}, g_bytesToHost{0}, g_emptyPacketsToHost{0};
std::atomic<int64_t> g_lastFromHostUs{0};
std::atomic<bool> g_windowsHost{false};  // it asked for a Microsoft OS descriptor (feedback format)
std::atomic<uint32_t> g_playbackRestarts{0};
Logic::RateMatch g_rateMatch;            // adaptive playback (the firmware's audio task)
int16_t g_lastFrame[2] = {};
constexpr int16_t kVolumeMin = -90 * 256;
constexpr int16_t kVolumeStep = 256;

using detail::Way;

// The system channel: commands in, answers and console text out. An answer goes back the way its command came.
std::atomic<ReportHandler> g_appHandler{nullptr};
std::atomic<Way> g_appWay{Way::Hid};  // the last app report came this way: answers go back so
// MIDI packets come from two tasks (the firmware's MidiWrite, the USB task's SysEx answers and console): one
// writer at a time, so a SysEx reaches the host whole.
SemaphoreHandle_t g_midiTx = nullptr;
struct MidiTxLock
{
    MidiTxLock() { if (g_midiTx != nullptr) xSemaphoreTake(g_midiTx, portMAX_DELAY); }
    ~MidiTxLock() { if (g_midiTx != nullptr) xSemaphoreGive(g_midiTx); }
};
std::atomic<bool> g_consoleOn{false};
std::atomic<Way> g_consoleWay{Way::Hid};  // the system channel's console goes out this way (as it was asked)
std::atomic<bool> g_hostTalked{false};    // a system command arrived (Config::fallbackSeconds)
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;

// The console: the last kilobyte printed, read by the system channel's console and by the serial port, each
// from its own place (a reader that falls behind loses the oldest text).
constexpr std::size_t kConsoleRing = 1024;
char g_console[kConsoleRing];
uint32_t g_consoleTotal = 0;       // bytes ever printed
uint32_t g_consoleSystemAt = 0;    // the system channel's console has sent up to here
#if CFG_TUD_CDC
uint32_t g_consoleSerialAt = 0;    // the serial port has sent up to here
#endif

struct PendingReport {
    uint8_t id;
    uint8_t data[kReportSize];
};
constexpr std::size_t kPendingReports = 4;
[[maybe_unused]] PendingReport g_pending[kPendingReports];
[[maybe_unused]] std::size_t g_pendingHead = 0, g_pendingCount = 0;
std::atomic<int> g_restart{0};  // a command asked for it; done from the USB task after the answer
// Set before the fallback restart (Config::fallbackSeconds): the next start keeps USB-Serial-JTAG.
constexpr uint32_t kSkipUsbOnce = 0x55534246;
RTC_NOINIT_ATTR uint32_t g_skipUsb;

// MIDI from the host, minus the system SysEx the HAL answers itself. Only the USB task writes it.
constexpr std::size_t kMidiQueue = 64;
uint8_t g_midi[kMidiQueue][4];
std::atomic<std::size_t> g_midiHead{0}, g_midiTail{0};
#if CFG_TUD_MIDI
// A system SysEx being collected (F0 23 ... F7): what follows F0 23.
uint8_t g_sysex[kReportSize];
std::size_t g_sysexLength = 0;
bool g_sysexOurs = false, g_sysexActive = false;
#endif

void StoreConsole(const char* data, std::size_t length)
{
    portENTER_CRITICAL_SAFE(&g_lock);
    for (std::size_t index = 0; index < length; ++index) g_console[(g_consoleTotal + index) % kConsoleRing] = data[index];
    g_consoleTotal += static_cast<uint32_t>(length);
    portEXIT_CRITICAL_SAFE(&g_lock);
}

// Up to `capacity` console bytes from `at` (moved on). Call with g_lock held.
std::size_t TakeConsoleLocked(uint32_t& at, char* out, std::size_t capacity)
{
    if (g_consoleTotal - at > kConsoleRing) at = g_consoleTotal - static_cast<uint32_t>(kConsoleRing);
    const std::size_t length = std::min<std::size_t>(capacity, g_consoleTotal - at);
    for (std::size_t index = 0; index < length; ++index) out[index] = g_console[(at + index) % kConsoleRing];
    at += static_cast<uint32_t>(length);
    return length;
}

[[maybe_unused]] std::size_t TakeConsole(uint32_t& at, char* out, std::size_t capacity)
{
    portENTER_CRITICAL_SAFE(&g_lock);
    const std::size_t length = TakeConsoleLocked(at, out, capacity);
    portEXIT_CRITICAL_SAFE(&g_lock);
    return length;
}

// ---- HID ------------------------------------------------------------------------------------------------------

bool QueueReport(uint8_t id, const uint8_t* data, std::size_t length)
{
#if CFG_TUD_HID
    if (!g_classes.hid) return false;
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
#else
    (void)id;
    (void)data;
    (void)length;
    return false;
#endif
}

#if CFG_TUD_HID
// HID inputs (usb/hid.h): keyboard, media keys and gamepad reports, in order, beside the system channel's.
struct InputReport {
    uint8_t id;
    uint8_t length;
    uint8_t data[14];
};
constexpr std::size_t kInputReports = 16;
InputReport g_inputs[kInputReports];
std::size_t g_inputHead = 0, g_inputCount = 0;

// The next report: a firmware's input first, then the system channel's queue, then console text.
bool NextReport(PendingReport& out, uint8_t& length)
{
    bool any = false;
    portENTER_CRITICAL_SAFE(&g_lock);
    if (g_inputCount > 0)
    {
        const InputReport& input = g_inputs[g_inputHead];
        out.id = input.id;
        std::memcpy(out.data, input.data, input.length);
        length = input.length;
        g_inputHead = (g_inputHead + 1) % kInputReports;
        --g_inputCount;
        any = true;
    }
    else if (g_pendingCount > 0)
    {
        out = g_pending[g_pendingHead];
        length = kReportSize;
        g_pendingHead = (g_pendingHead + 1) % kPendingReports;
        --g_pendingCount;
        any = true;
    }
    else if (g_consoleOn.load(std::memory_order_relaxed) && g_consoleWay.load(std::memory_order_relaxed) == Way::Hid &&
             g_consoleTotal != g_consoleSystemAt)
    {
        out.id = kReportSystem;
        std::memset(out.data, 0, kReportSize);
        out.data[0] = kCommandConsole | 0x80;
        const std::size_t taken = TakeConsoleLocked(g_consoleSystemAt, reinterpret_cast<char*>(out.data + 2), kReportSize - 2);
        out.data[1] = static_cast<uint8_t>(taken);
        length = kReportSize;
        any = true;
    }
    portEXIT_CRITICAL_SAFE(&g_lock);
    return any;
}

// The report on its way, kept until the endpoint takes it. USB task only.
PendingReport g_outgoing;
uint8_t g_outgoingLength = 0;
bool g_outgoingReady = false;

void PumpReports()
{
    if (!g_classes.hid || !tud_hid_ready()) return;
    if (!g_outgoingReady) g_outgoingReady = NextReport(g_outgoing, g_outgoingLength);
    if (g_outgoingReady && tud_hid_report(g_outgoing.id, g_outgoing.data, g_outgoingLength)) g_outgoingReady = false;
}
#endif

// ---- MIDI and SysEx -----------------------------------------------------------------------------------------

#if CFG_TUD_MIDI
// A whole SysEx (F0 … F7) in USB-MIDI packets of up to three bytes, under the MIDI lock. A packet that finds
// no room waits up to `waitMs` (0: it is dropped, as it always was for the short answers); false when one could
// not go.
bool WriteSysex(const uint8_t* message, std::size_t size, uint32_t waitMs)
{
    MidiTxLock lock;
    bool whole = true;
    for (std::size_t at = 0; at < size; at += 3)
    {
        const std::size_t left = size - at;
        uint8_t packet[4] = {0x04, 0, 0, 0};  // SysEx starts or continues
        if (left <= 3) packet[0] = static_cast<uint8_t>(0x04 + left);  // ends with 1, 2 or 3 bytes: 0x5, 0x6, 0x7
        for (std::size_t index = 0; index < 3 && index < left; ++index) packet[1 + index] = message[at + index];
        bool written = tud_midi_packet_write(packet);
        for (uint32_t waited = 0; !written && waited < waitMs && Mounted(); ++waited)
        {
            vTaskDelay(1);
            written = tud_midi_packet_write(packet);
        }
        if (!written && waitMs != 0) return false;
        whole = whole && written;
    }
    return whole;
}
#endif

// An answer as SysEx: F0 24 <bytes, 7-bit> F7. False when a packet found the MIDI FIFO full (the host
// stopped reading): that SysEx did not arrive whole.
bool SendSysex(const uint8_t* data, std::size_t length)
{
#if CFG_TUD_MIDI
    if (!g_classes.midi) return false;
    uint8_t message[kReportSize + 3];
    std::size_t size = 0;
    message[size++] = 0xF0;
    message[size++] = 0x24;
    for (std::size_t index = 0; index < length && size < sizeof(message) - 1; ++index) message[size++] = data[index] & 0x7F;
    message[size++] = 0xF7;
    return WriteSysex(message, size, 0);
#else
    (void)data;
    (void)length;
    return false;
#endif
}

// ---- WebUSB ---------------------------------------------------------------------------------------------------

#if CARDENZA_USB_BUILD_WEBUSB
// Answers waiting for the page to ask (kWebUsbReceive): [report ID, 63 bytes].
constexpr std::size_t kWebUsbReports = 8;
uint8_t g_webUsbQueue[kWebUsbReports][1 + kReportSize];
std::size_t g_webUsbHead = 0, g_webUsbCount = 0;
uint8_t g_webUsbIn[1 + kReportSize] __attribute__((aligned(4)));   // a request's data
uint8_t g_webUsbOut[1 + kReportSize] __attribute__((aligned(4)));  // the answer being read
#endif

bool QueueWebUsb(uint8_t id, const uint8_t* data, std::size_t length)
{
#if CARDENZA_USB_BUILD_WEBUSB
    if (!g_classes.webusb) return false;
    bool queued = false;
    portENTER_CRITICAL_SAFE(&g_lock);
    if (g_webUsbCount < kWebUsbReports)
    {
        uint8_t* slot = g_webUsbQueue[(g_webUsbHead + g_webUsbCount) % kWebUsbReports];
        slot[0] = id;
        std::memset(slot + 1, 0, kReportSize);
        std::memcpy(slot + 1, data, std::min(length, kReportSize));
        ++g_webUsbCount;
        queued = true;
    }
    portEXIT_CRITICAL_SAFE(&g_lock);
    return queued;
#else
    (void)id;
    (void)data;
    (void)length;
    return false;
#endif
}

// ---- The serial port ------------------------------------------------------------------------------------------

#if CFG_TUD_CDC
// What goes out on the serial port besides the console: answers, whole lines (a text answer may come in pieces;
// the console waits until it is done).
constexpr std::size_t kSerialOut = 1024;
char g_serialOut[kSerialOut];
std::size_t g_serialOutHead = 0, g_serialOutCount = 0;
std::atomic<bool> g_serialAnswerOpen{false};  // a text answer is under way
bool g_serialTextWritten = false;             // it has written something (dev worker only)
std::atomic<bool> g_serialLog{true};          // the console goes to the serial port
bool g_serialAtLineStart = true;              // USB task only
bool g_serialLastWasConsole = false;
Logic::LineReader g_serialLine;
Logic::ResetDetector g_serialReset;
#endif

// Appends to the serial port's output, all or nothing, waiting up to `waitMs` for room.
bool SerialAppend(const char* text, std::size_t length, uint32_t waitMs)
{
#if CFG_TUD_CDC
    if (length > kSerialOut) return false;
    for (uint32_t waited = 0;; ++waited)
    {
        if (!g_classes.serial || !Mounted()) return false;
        bool done = false;
        portENTER_CRITICAL_SAFE(&g_lock);
        if (kSerialOut - g_serialOutCount >= length)
        {
            for (std::size_t index = 0; index < length; ++index)
                g_serialOut[(g_serialOutHead + g_serialOutCount + index) % kSerialOut] = text[index];
            g_serialOutCount += length;
            done = true;
        }
        portEXIT_CRITICAL_SAFE(&g_lock);
        if (done) return true;
        if (waited >= waitMs) return false;
        vTaskDelay(1);
    }
#else
    (void)text;
    (void)length;
    (void)waitMs;
    return false;
#endif
}

#if CFG_TUD_CDC
bool SerialLine(const char* text, uint32_t waitMs = 0)
{
    char line[200];
    const int length = std::snprintf(line, sizeof(line), "%s\n", text);
    return length > 0 && SerialAppend(line, std::min<std::size_t>(length, sizeof(line) - 1), waitMs);
}
#endif

bool SerialHex(const char* word, const uint8_t* bytes, std::size_t length, uint32_t waitMs)
{
    char line[8 + 2 * kReportSize + 2];
    const std::size_t size = Logic::HexLine(word, bytes, std::min(length, kReportSize), line);
    return SerialAppend(line, size, waitMs);
}

// ---- Commands -------------------------------------------------------------------------------------------------

void Answer(Way way, const uint8_t* data, std::size_t length)
{
    uint8_t report[kReportSize] = {};
    std::memcpy(report, data, std::min(length, kReportSize));
    switch (way)
    {
        case Way::Hid: QueueReport(kReportSystem, data, length); break;
        case Way::Sysex: SendSysex(data, length); break;
        case Way::Serial: SerialHex("sys", report, kReportSize, 0); break;
        case Way::WebUsb: QueueWebUsb(kReportSystem, report, kReportSize); break;
        case Way::Text: break;
    }
}

void HandleApp(const uint8_t* data, std::size_t length, Way way)
{
    g_appWay.store(way, std::memory_order_relaxed);
    if (ReportHandler handler = g_appHandler.load(std::memory_order_relaxed)) handler(data, length);
}

void HandleCommand(const uint8_t* data, std::size_t length, Way way)
{
    if (length == 0) return;
    g_hostTalked.store(true, std::memory_order_relaxed);
    const uint8_t command = data[0];
    switch (command)
    {
        case kCommandIdentify:
        {
            uint8_t answer[kReportSize] = {static_cast<uint8_t>(command | 0x80), kProtocolVersion};
            std::strncpy(reinterpret_cast<char*>(answer + 2), detail::Product(), kReportSize - 3);
            Answer(way, answer, 2 + std::strlen(reinterpret_cast<char*>(answer + 2)));
            return;
        }
        case kCommandBootloader:
        case kCommandReboot:
        case kCommandDownload:
        {
            const uint8_t answer[1] = {static_cast<uint8_t>(command | 0x80)};
            Answer(way, answer, sizeof(answer));
            g_restart.store(command, std::memory_order_relaxed);
            return;
        }
        case kCommandDev:
            detail::DevRequest(data + 1, length - 1, way);
            return;
        case kCommandConsole:
        {
            const bool on = length > 1 && data[1] != 0;
#if CFG_TUD_CDC
            if (way == Way::Serial)
            {
                g_serialLog.store(on, std::memory_order_relaxed);
                return;
            }
#endif
            if (on)
            {
                portENTER_CRITICAL_SAFE(&g_lock);
                if (g_consoleTotal - g_consoleSystemAt > kConsoleRing) g_consoleSystemAt = g_consoleTotal - kConsoleRing;
                portEXIT_CRITICAL_SAFE(&g_lock);
            }
            g_consoleWay.store(way, std::memory_order_relaxed);
            g_consoleOn.store(on, std::memory_order_relaxed);
            return;
        }
        case kCommandApp:
            if (way == Way::Hid) return;  // over HID the firmware has its own report ID
            HandleApp(data + 1, length - 1, way);
            return;
        default:
            return;
    }
}

#if CFG_TUD_MIDI
// Console text as SysEx (F0 24 C3 <text> F7), when the host asked for it that way.
void PumpSysexConsole()
{
    if (!g_consoleOn.load(std::memory_order_relaxed) || g_consoleWay.load(std::memory_order_relaxed) != Way::Sysex) return;
    uint8_t text[kReportSize];
    const std::size_t length = TakeConsole(g_consoleSystemAt, reinterpret_cast<char*>(text + 1), 48);
    if (length == 0) return;
    text[0] = kCommandConsole | 0x80;
    // A host that stopped reading MIDI gets no more console until it asks again: the console would fill
    // the FIFO with cut SysEx in front of the firmware's own MIDI.
    if (!SendSysex(text, length + 1)) g_consoleOn.store(false, std::memory_order_relaxed);
}
#endif

// Console text over WebUSB (0xC3 reports), while the page reads them.
void PumpWebUsbConsole()
{
#if CARDENZA_USB_BUILD_WEBUSB
    if (!g_consoleOn.load(std::memory_order_relaxed) || g_consoleWay.load(std::memory_order_relaxed) != Way::WebUsb) return;
    portENTER_CRITICAL_SAFE(&g_lock);
    const bool room = g_webUsbCount < kWebUsbReports;
    portEXIT_CRITICAL_SAFE(&g_lock);
    if (!room) return;
    uint8_t report[kReportSize] = {static_cast<uint8_t>(kCommandConsole | 0x80)};
    const std::size_t length = TakeConsole(g_consoleSystemAt, reinterpret_cast<char*>(report + 2), kReportSize - 2);
    if (length == 0) return;
    report[1] = static_cast<uint8_t>(length);
    QueueWebUsb(kReportSystem, report, kReportSize);
#endif
}

#if CFG_TUD_MIDI
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
                if (g_sysexOurs) HandleCommand(g_sysex, g_sysexLength, Way::Sysex);
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
#endif

#if CFG_TUD_CDC
void SerialHelp()
{
    static const char* const kLines[] = {
        "Commands:",
        "  version                  Firmware <name> <version>",
        "  info                     the firmware, the HAL, the chip (JSON)",
        "  hello | partitions | memory | storage | status | i2c | settings | coredump   (JSON)",
        "  launcher                 restart into the 203 Launcher",
        "  download                 restart into the ROM download mode (esptool)",
        "  reboot                   restart the firmware",
        "  log on | log off         the console on this port",
        "  sys <hex> | app <hex>    a report 0xCB / 0xFF (docs/usb-dev.md)",
        "OK",
    };
    for (const char* line : kLines) SerialLine(line);
}

void HandleLine(const char* line)
{
    struct Word {
        const char* word;
        uint8_t op;
    };
    static constexpr Word kJson[] = {
        {"info", Dev::kInfo},       {"hello", Dev::kHello},   {"partitions", Dev::kPartitions}, {"memory", Dev::kMemory},
        {"storage", Dev::kStorage}, {"status", Dev::kStatus}, {"i2c", Dev::kI2c},               {"settings", Dev::kSettings},
        {"coredump", Dev::kCoredumpInfo},
    };
    g_hostTalked.store(true, std::memory_order_relaxed);
    if (Logic::SameWords(line, "version"))
    {
        const esp_app_desc_t* app = esp_app_get_description();
        char text[96];
        std::snprintf(text, sizeof(text), "Firmware %.32s %.32s", app->project_name, app->version);
        SerialLine(text);
        return;
    }
    for (const Word& entry : kJson)
        if (Logic::SameWords(line, entry.word))
        {
            const uint8_t request[2] = {0, entry.op};
            detail::DevRequest(request, sizeof(request), Way::Text);
            return;
        }
    if (Logic::SameWords(line, "help")) return SerialHelp();
    if (Logic::SameWords(line, "log on") || Logic::SameWords(line, "log off"))
    {
        const bool on = Logic::SameWords(line, "log on");
        g_serialLog.store(on, std::memory_order_relaxed);
        SerialLine(on ? "OK log on" : "OK log off");
        return;
    }
    if (Logic::SameWords(line, "launcher") || Logic::SameWords(line, "download") || Logic::SameWords(line, "reboot"))
    {
        const bool launcher = Logic::SameWords(line, "launcher");
        const bool download = Logic::SameWords(line, "download");
        SerialLine(launcher ? "OK launcher" : download ? "OK download" : "OK reboot");
        g_restart.store(launcher ? kCommandBootloader : download ? kCommandDownload : kCommandReboot, std::memory_order_relaxed);
        return;
    }
    uint8_t bytes[kReportSize];
    int count = Logic::HexArgument(line, "sys", bytes, sizeof(bytes));
    if (count > 0) return HandleCommand(bytes, static_cast<std::size_t>(count), Way::Serial);
    count = Logic::HexArgument(line, "app", bytes, sizeof(bytes));
    if (count >= 0) return HandleApp(bytes, static_cast<std::size_t>(count), Way::Serial);
    SerialLine("ERR unknown command, type 'help' for command list");
}

// The serial port: lines in; answers, then the console, out. Never waits.
void PumpSerial()
{
    if (!g_classes.serial || !tud_mounted()) return;
    uint8_t in[64];
    while (tud_cdc_available())
    {
        const uint32_t count = tud_cdc_read(in, sizeof(in));
        if (count == 0) break;
        for (uint32_t index = 0; index < count; ++index)
            if (g_serialLine.Push(static_cast<char>(in[index]))) HandleLine(g_serialLine.Line());
    }
    for (int round = 0; round < 4; ++round)
    {
        uint32_t room = tud_cdc_write_available();
        if (room == 0) break;
        char out[64];
        std::size_t length = 0;
        bool fromConsole = false;
        portENTER_CRITICAL_SAFE(&g_lock);
        if (g_serialOutCount > 0)
        {
            // An answer after console text that stopped mid-line starts on a line of its own.
            if (g_serialLastWasConsole && !g_serialAtLineStart) out[length++] = '\n';
            while (length < sizeof(out) && length < room && g_serialOutCount > 0)
            {
                out[length++] = g_serialOut[g_serialOutHead];
                g_serialOutHead = (g_serialOutHead + 1) % kSerialOut;
                --g_serialOutCount;
            }
        }
        else if (g_serialLog.load(std::memory_order_relaxed) && !g_serialAnswerOpen.load(std::memory_order_relaxed))
        {
            length = TakeConsoleLocked(g_consoleSerialAt, out, std::min<std::size_t>(sizeof(out), room));
            fromConsole = true;
        }
        portEXIT_CRITICAL_SAFE(&g_lock);
        if (length == 0) break;
        tud_cdc_write(out, static_cast<uint32_t>(length));
        g_serialLastWasConsole = fromConsole;
        g_serialAtLineStart = out[length - 1] == '\n';
    }
    tud_cdc_write_flush();
}
#endif

// ---- The console as a file: stdout and stderr are reopened on it ------------------------------------------------

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

bool g_consoleRedirected = false;

void RedirectConsole()
{
    if (g_consoleRedirected) return;
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
    g_consoleRedirected = true;
}

// ---- Descriptors ----------------------------------------------------------------------------------------------

#if CARDENZA_USB_BUILD_WEBUSB
// The interface's GUID for WinUSB (MS OS 2.0, DeviceInterfaceGUIDs): fixed, so a Windows tool can find it.
constexpr char kWebUsbGuid[] = "{3C0D4A20-0203-4C5A-B1E5-CA4DE2A0B0E1}";

void BuildMsOs20(uint8_t itf, uint8_t* out)
{
    std::size_t at = 0;
    const auto u16 = [&](uint16_t value) {
        out[at++] = static_cast<uint8_t>(value);
        out[at++] = static_cast<uint8_t>(value >> 8);
    };
    const auto wide = [&](const char* text, std::size_t nulls) {
        for (; *text != '\0'; ++text) u16(static_cast<uint8_t>(*text));
        for (std::size_t index = 0; index < nulls; ++index) u16(0);
    };
    // Set header, version Windows 8.1.
    u16(0x000A);
    u16(MS_OS_20_SET_HEADER_DESCRIPTOR);
    u16(0x0000);
    u16(0x0603);
    u16(kMsOs20Length);
    // Configuration subset (the configuration's index, 0).
    u16(0x0008);
    u16(MS_OS_20_SUBSET_HEADER_CONFIGURATION);
    out[at++] = 0;
    out[at++] = 0;
    u16(kMsOs20Length - 0x0A);
    // Function subset: the WebUSB interface.
    u16(0x0008);
    u16(MS_OS_20_SUBSET_HEADER_FUNCTION);
    out[at++] = itf;
    out[at++] = 0;
    u16(kMsOs20Length - 0x0A - 0x08);
    // Compatible ID: WinUSB.
    u16(0x0014);
    u16(MS_OS_20_FEATURE_COMPATBLE_ID);
    const char compatible[16] = {'W', 'I', 'N', 'U', 'S', 'B'};
    std::memcpy(out + at, compatible, sizeof(compatible));
    at += sizeof(compatible);
    // Registry property: DeviceInterfaceGUIDs (REG_MULTI_SZ).
    u16(static_cast<uint16_t>(kMsOs20Length - 0x0A - 0x08 - 0x08 - 0x14));
    u16(MS_OS_20_FEATURE_REG_PROPERTY);
    u16(0x0007);
    u16(0x002A);
    wide("DeviceInterfaceGUIDs", 1);
    u16(0x0050);
    wide(kWebUsbGuid, 2);
}
#endif

// The classes' descriptors for `classes`, into `out`.
void BuildDescriptors(const Logic::Classes& classes, const Logic::Endpoints& ep, const Config& config, Descriptors& out)
{
    std::memset(&out, 0, sizeof(out));
    out.itfAudio = 0xFF;
    out.itfWebUsb = 0xFF;
    uint8_t interfaces = 0;
    uint8_t* at = out.configuration + TUD_CONFIG_DESC_LEN;
    uint8_t* const end = out.configuration + sizeof(out.configuration);
    const auto append = [&](const uint8_t* block, std::size_t length) {
        if (at + length > end) return;
        std::memcpy(at, block, length);
        at += length;
    };
#if CFG_TUD_AUDIO
    if (classes.audio)
    {
        out.itfAudio = interfaces;
        out.audioPlayback = classes.playback;
        if (!classes.playback)
        {
            const uint8_t block[] = {CARDENZA_USB_AUDIO_DESCRIPTOR_RECORD(interfaces, kStringAudio, ep.audioIn)};
            append(block, sizeof(block));
            interfaces += 2;
        }
        else if (classes.feedback)
        {
            const uint8_t block[] = {CARDENZA_USB_AUDIO_DESCRIPTOR(interfaces, kStringAudio, ep.audioOut, ep.audioFeedback, ep.audioIn)};
            append(block, sizeof(block));
            interfaces += 3;
        }
        else
        {
            const uint8_t block[] = {CARDENZA_USB_AUDIO_DESCRIPTOR_ADAPTIVE(interfaces, kStringAudio, ep.audioOut, ep.audioIn)};
            append(block, sizeof(block));
            interfaces += 3;
        }
    }
#endif
#if CFG_TUD_MIDI
    if (classes.midi)
    {
        // Its own interface association: next to a UAC2 function macOS does not take the MIDI interfaces
        // without one.
        const uint8_t association[] = {8, TUSB_DESC_INTERFACE_ASSOCIATION, interfaces, 2, TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_CONTROL, AUDIO_FUNC_PROTOCOL_CODE_UNDEF, 0};
        append(association, sizeof(association));
        const uint8_t block[] = {TUD_MIDI_DESCRIPTOR(interfaces, kStringMidi, ep.midiOut, ep.midiIn, 64)};
        append(block, sizeof(block));
        interfaces += 2;
    }
#endif
#if CFG_TUD_HID
    if (classes.hid)
    {
        // The firmware's inputs (keyboard, media keys, gamepad), then the system and app channels.
        out.hidReportLength = static_cast<uint16_t>(
            detail::HidInputReportDescriptor(out.hidReport, sizeof(out.hidReport), config.keyboard, config.consumer, config.gamepad));
        for (const uint8_t id : {kReportSystem, kReportApp})
        {
            const uint8_t block[] = {TUD_HID_REPORT_DESC_GENERIC_INOUT(kReportSize, HID_REPORT_ID(id))};
            if (out.hidReportLength + sizeof(block) > sizeof(out.hidReport)) break;
            std::memcpy(out.hidReport + out.hidReportLength, block, sizeof(block));
            out.hidReportLength = static_cast<uint16_t>(out.hidReportLength + sizeof(block));
        }
        const uint8_t block[] = {TUD_HID_INOUT_DESCRIPTOR(interfaces, kStringHid, HID_ITF_PROTOCOL_NONE, out.hidReportLength, ep.hidOut, ep.hidIn, 64, 1)};
        append(block, sizeof(block));
        interfaces += 1;
    }
#endif
#if CFG_TUD_CDC
    if (classes.serial)
    {
        const uint8_t block[] = {TUD_CDC_DESCRIPTOR(interfaces, kStringSerialPort, ep.serialNotify, 64, ep.serialOut, ep.serialIn, 64)};
        append(block, sizeof(block));
        interfaces += 2;
    }
#endif
#if CARDENZA_USB_BUILD_WEBUSB
    if (classes.webusb)
    {
        out.itfWebUsb = interfaces;
        // The screen stream (the camera, usb/camera.h) is this interface's bulk IN endpoint, when it is on.
        const uint8_t block[] = {9, TUSB_DESC_INTERFACE, interfaces, 0, static_cast<uint8_t>(classes.camera ? 1 : 0),
                                 TUSB_CLASS_VENDOR_SPECIFIC, 0x00, 0x00, kStringWebUsb};
        append(block, sizeof(block));
        if (classes.camera)
        {
            const uint8_t endpoint[] = {7, TUSB_DESC_ENDPOINT, ep.cameraIn, TUSB_XFER_BULK, U16_TO_U8S_LE(64), 0};
            append(endpoint, sizeof(endpoint));
        }
        interfaces += 1;
        out.webusb = true;
        const uint8_t bos[] = {TUD_BOS_DESCRIPTOR(kBosLength, 2), TUD_BOS_WEBUSB_DESCRIPTOR(kVendorWebUsb, 1),
                               TUD_BOS_MS_OS_20_DESCRIPTOR(kMsOs20Length, kVendorMicrosoft)};
        static_assert(sizeof(bos) == kBosLength, "BOS length");
        std::memcpy(out.bos, bos, sizeof(bos));
        BuildMsOs20(out.itfWebUsb, out.msOs20);
    }
#endif
    if (classes.drive)
    {
        const std::size_t length = detail::DriveDescriptor(at, static_cast<std::size_t>(end - at), interfaces, kStringDrive, ep.driveOut, ep.driveIn);
        if (length != 0)
        {
            at += length;
            interfaces += 1;
        }
    }
    if (classes.network)
    {
        const std::size_t length = detail::NetworkDescriptor(at, static_cast<std::size_t>(end - at), interfaces, kStringNetwork,
                                                             kStringNetworkMac, ep.networkNotify, ep.networkOut, ep.networkIn);
        if (length != 0)
        {
            at += length;
            interfaces += 2;
        }
    }
    out.configurationLength = static_cast<uint16_t>(at - out.configuration);
    const uint8_t header[] = {TUD_CONFIG_DESCRIPTOR(1, interfaces, 0, out.configurationLength, 0x00, 500)};
    std::memcpy(out.configuration, header, sizeof(header));

    out.device.bLength = sizeof(tusb_desc_device_t);
    out.device.bDescriptorType = TUSB_DESC_DEVICE;
    // 2.1 tells the computer to read the BOS (WebUSB, Microsoft OS 2.0).
    out.device.bcdUSB = out.webusb ? 0x0210 : 0x0200;
    // Interface association descriptors (audio, MIDI, the serial port and the camera have one).
    out.device.bDeviceClass = TUSB_CLASS_MISC;
    out.device.bDeviceSubClass = MISC_SUBCLASS_COMMON;
    out.device.bDeviceProtocol = MISC_PROTOCOL_IAD;
    out.device.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE;
    out.device.idVendor = config.vendorId;
    out.device.idProduct = config.productId;
    out.device.bcdDevice = config.deviceVersion;
    out.device.iManufacturer = kStringManufacturer;
    out.device.iProduct = kStringProduct;
    out.device.iSerialNumber = kStringSerial;
    out.device.bNumConfigurations = 1;
}

bool SameDescriptors(const Descriptors& a, const Descriptors& b)
{
    return std::memcmp(&a.device, &b.device, sizeof(a.device)) == 0 && a.configurationLength == b.configurationLength &&
           std::memcmp(a.configuration, b.configuration, a.configurationLength) == 0 && a.hidReportLength == b.hidReportLength &&
           std::memcmp(a.hidReport, b.hidReport, a.hidReportLength) == 0 && a.webusb == b.webusb;
}

void ClassList(const Logic::Classes& classes, char* out, std::size_t capacity)
{
    std::snprintf(out, capacity, "%s%s%s%s%s%s%s%s", classes.midi ? " midi" : "",
                  classes.audio ? (!classes.playback ? " audio(recording)" : classes.feedback ? " audio" : " audio(adaptive)") : "", classes.hid ? " hid" : "",
                  classes.webusb ? " webusb" : "", classes.serial ? " serial" : "", classes.drive ? " drive" : "",
                  classes.camera ? " camera" : "", classes.network ? " network" : "");
}

// ---- Audio's playback watch ---------------------------------------------------------------------------------

#if CFG_TUD_AUDIO
// TinyUSB's DWC2 driver retries an isochronous IN transfer that missed its frame, but not an OUT one:
// re-armed a frame late (the USB interrupt held up, by a flash write for one), the host's playback
// endpoint waits for a frame parity that has passed and never receives again. While the host plays
// and nothing has arrived for 20 frames, it is activated afresh and the next packet scheduled.
constexpr int64_t kPlaybackStallUs = 20000;

void WatchPlayback()
{
    if (!g_classes.audio || !g_playing.load(std::memory_order_relaxed)) return;
    const int64_t now = esp_timer_get_time();
    if (now - g_lastFromHostUs.load(std::memory_order_relaxed) < kPlaybackStallUs) return;
    tu_fifo_t* fifo = tud_audio_get_ep_out_ff();
    if (fifo == nullptr) return;
    g_lastFromHostUs.store(now, std::memory_order_relaxed);  // one attempt per stall time
    const uint8_t endpoint[] = {7, TUSB_DESC_ENDPOINT, g_ep.audioOut,
                                (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS |
                                          (uint8_t)(g_classes.feedback ? TUSB_ISO_EP_ATT_ASYNCHRONOUS : TUSB_ISO_EP_ATT_ADAPTIVE) |
                                          (uint8_t)TUSB_ISO_EP_ATT_DATA),
                                U16_TO_U8S_LE(CARDENZA_USB_AUDIO_EP_SIZE), 1};
    const auto* descriptor = reinterpret_cast<const tusb_desc_endpoint_t*>(endpoint);
    if (usbd_edpt_iso_activate(0, descriptor) && usbd_edpt_xfer_fifo(0, g_ep.audioOut, fifo, CARDENZA_USB_AUDIO_EP_SIZE, false))
        g_playbackRestarts.fetch_add(1, std::memory_order_relaxed);
}
#endif

// ---- Restarts -------------------------------------------------------------------------------------------------

void LeaveHost()
{
    if (g_started.load())
    {
        tud_disconnect();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

[[noreturn]] void Restart()
{
    LeaveHost();
    // USB-Serial-JTAG gets the PHY back, for whatever starts next (the Launcher's console, esptool).
    RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 0;
    esp_restart();
}

// After a start that failed: the PHY goes back to USB-Serial-JTAG, as it was before Init, so the console
// and esptool's port come back.
void ReleasePhy()
{
    usb_del_phy(g_phy);
    g_phy = nullptr;
    RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 0;
}

void PumpAll()
{
#if CFG_TUD_MIDI
    if (g_classes.midi)
    {
        PumpMidi();
        PumpSysexConsole();
    }
#endif
#if CFG_TUD_HID
    detail::HidInputPump();
    PumpReports();
#endif
#if CFG_TUD_CDC
    PumpSerial();
#endif
    PumpWebUsbConsole();
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
        // The HAL's own start: connect once its second is up, unless the firmware took the device over first.
        const int64_t connectAt = g_connectAtUs.load(std::memory_order_relaxed);
        if (connectAt != 0 && esp_timer_get_time() >= connectAt && xSemaphoreTake(g_configLock, 0) == pdTRUE)
        {
            if (g_connectAtUs.exchange(0) != 0) tud_connect();
            xSemaphoreGive(g_configLock);
        }
        tud_task_ext(1, false);  // at most a millisecond, so the reports keep moving
        PumpAll();
#if CFG_TUD_AUDIO
        WatchPlayback();
#endif
        const int restart = g_restart.exchange(0, std::memory_order_relaxed);
        if (restart != 0)
        {
            // Let the answer go out first (Matrix OS waits 20 ms too; a page reading WebUSB asks every few).
            for (int wait = 0; wait < 40; ++wait)
            {
                tud_task_ext(1, false);
                PumpAll();
            }
            if (restart == kCommandDownload) RestartIntoDownloadMode();
            if (restart == kCommandBootloader) RestartIntoLauncher();
            Restart();
        }
    }
}

// The classes and their descriptors from now on. The device is disconnected around a change the computer
// would see; with g_configLock held.
bool Apply(const Config& config, const Logic::Classes& asked, const Logic::Classes& keep, bool log)
{
    const Logic::Classes effective = Logic::Effective(asked, kBuilt);
    const Logic::Fitted fitted = Logic::Fit(effective, keep);
    const Logic::Endpoints endpoints = Logic::Allocate(fitted.classes);
    if (!endpoints.fits)
    {
        char wanted[96];
        ClassList(fitted.classes, wanted, sizeof(wanted));
        ESP_LOGE(kTag, "classes%s need %d IN endpoints (the ESP32-S3 has %d): turn one off", wanted, endpoints.inCount, Logic::kInEndpoints);
        return false;
    }
    BuildDescriptors(fitted.classes, endpoints, config, g_next);
    const bool connected = g_started.load() && g_connectAtUs.load() == 0;
    const bool same = SameDescriptors(g_next, g_desc);
    if (connected && !same)
    {
        tud_disconnect();
        vTaskDelay(pdMS_TO_TICKS(150));  // long enough for every host to see the device go
    }
    g_desc = g_next;
    g_config = config;
    g_asked = asked;
    g_classes = fitted.classes;
    g_ep = endpoints;
    if (!same)
    {
        g_playing.store(false);
        g_recording.store(false);
        g_rateMatch.Reset();
        detail::HidInputReset();
    }
    if (connected && !same) tud_connect();
    if (log)
    {
        char list[96];
        ClassList(g_classes, list, sizeof(list));
        ESP_LOGI(kTag, "%04x:%04x %s, classes%s (%d of %d IN endpoints)", config.vendorId, config.productId, g_serial, list,
                 endpoints.inCount, Logic::kInEndpoints);
        const Logic::Classes& gone = fitted.dropped;
        if (gone.feedback || gone.hid || gone.serial || gone.audio || gone.midi)
            ESP_LOGW(kTag, "gave way for room:%s%s%s%s%s", gone.feedback ? " the playback's feedback endpoint (adaptive now)" : "",
                     gone.hid ? " hid" : "", gone.serial ? " serial" : "", gone.audio ? " audio" : "", gone.midi ? " midi" : "");
    }
    return true;
}

// Starts the device. `connectAfterUs`: 0 connects at once, else the HAL's own start, which waits for the
// firmware's Init that long.
bool Start(const Config& config, int64_t connectAfterUs)
{
    if (g_started.load()) return false;
    if (g_skipUsb == kSkipUsbOnce || g_skippedThisBoot)
    {
        g_skipUsb = 0;
        g_skippedThisBoot = true;
        ESP_LOGW(kTag, "the last start found no host: USB stays on USB-Serial-JTAG this time");
        return false;
    }
    if (g_midiTx == nullptr) g_midiTx = xSemaphoreCreateMutex();
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    Logic::SerialNumber(mac, g_serial);
    if (!Apply(config, Asked(config), Logic::Classes{}, true)) return false;

    usb_phy_config_t phy = {};
    phy.controller = USB_PHY_CTRL_OTG;
    phy.target = USB_PHY_TARGET_INT;
    phy.otg_mode = USB_OTG_MODE_DEVICE;
    if (usb_new_phy(&phy, &g_phy) != ESP_OK)
    {
        ESP_LOGE(kTag, "USB PHY failed");
        return false;
    }
    if (!tusb_init())
    {
        ReleasePhy();
        ESP_LOGE(kTag, "TinyUSB failed");
        return false;
    }
    if (connectAfterUs != 0)
    {
        tud_disconnect();
        g_connectAtUs.store(esp_timer_get_time() + connectAfterUs);
    }
    if (xTaskCreatePinnedToCore(UsbTask, "usb", 4096, nullptr, 8, nullptr, 0) != pdPASS)
    {
        tusb_deinit(0);
        ReleasePhy();
        ESP_LOGE(kTag, "USB task failed");
        return false;
    }
    // Only once the device runs: a start that failed leaves the console where it was (and its error
    // there to read).
    if (config.console) RedirectConsole();
    g_started.store(true);
    // The system menu (system/menu.h) restarts the board too: it leaves the host first, as Restart does.
    System::detail::SetUsbStop([] { LeaveHost(); });
    return true;
}

}  // namespace

namespace detail {

// usb/drive.cpp defines it when the drive needs to know (a weak default otherwise).
__attribute__((weak)) void DriveSuspended(bool) {}

void AutoStart()
{
    if (g_configLock == nullptr) g_configLock = xSemaphoreCreateMutex();
    xSemaphoreTake(g_configLock, portMAX_DELAY);
    // No HID keyboard in the HAL's own start: on macOS a keyboard makes the computer ask which kind it is, and
    // lets a host tool open the HID interface (the system channel with it) only with the Input Monitoring
    // permission (seen on the board 2026-11-24: hidapi could not open the template's). A firmware that types
    // asks for it in its own USB::Init.
    Config config;
    config.keyboard = false;
    if (!g_started.load() && Start(config, 1000000)) g_auto.store(true);
    xSemaphoreGive(g_configLock);
}

bool SetClasses(const Logic::Classes& wanted, const Logic::Classes& keep)
{
    if (!g_started.load()) return false;
    xSemaphoreTake(g_configLock, portMAX_DELAY);
    const bool applied = Apply(g_config, wanted, keep, true);
    xSemaphoreGive(g_configLock);
    return applied;
}

Logic::Classes AskedClasses() { return g_asked; }

}  // namespace detail

bool Init(const Config& config)
{
    if (g_configLock == nullptr) g_configLock = xSemaphoreCreateMutex();
    xSemaphoreTake(g_configLock, portMAX_DELAY);
    bool started = false;
    if (!g_started.load())
        started = Start(config, 0);
    else if (g_auto.exchange(false))
    {
        // CardenzaHAL::Init started it: the firmware's configuration from now on. Within the HAL's second
        // the computer has not seen the device yet; later it sees it again with these classes.
        started = Apply(config, Asked(config), Logic::Classes{}, true);
        if (config.console) RedirectConsole();
        if (g_connectAtUs.exchange(0) != 0) tud_connect();
    }
    xSemaphoreGive(g_configLock);
    return started;
}

bool Reconfigure(const Config& config)
{
    if (!g_started.load()) return false;
    xSemaphoreTake(g_configLock, portMAX_DELAY);
    g_auto.store(false);
    // The drive and the camera stay as their own calls left them.
    Logic::Classes asked = Asked(config);
    asked.drive = g_asked.drive;
    asked.camera = g_asked.camera;
    const bool applied = Apply(config, asked, Logic::Classes{}, true);
    xSemaphoreGive(g_configLock);
    return applied;
}

Config CurrentConfig() { return g_config; }

Presented Classes()
{
    Presented presented;
    presented.midi = g_classes.midi;
    presented.audio = g_classes.audio;
    presented.audioPlayback = g_classes.audio && g_classes.playback;
    presented.audioFeedback = g_classes.audio && g_classes.playback && g_classes.feedback;
    presented.hid = g_classes.hid;
    presented.webusb = g_classes.webusb;
    presented.serial = g_classes.serial;
    presented.drive = g_classes.drive;
    presented.camera = g_classes.camera;
    presented.network = g_classes.network;
    return presented;
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
#if CFG_TUD_MIDI
    if (!Mounted() || !g_classes.midi) return false;
    MidiTxLock lock;
    return tud_midi_packet_write(packet);
#else
    (void)packet;
    return false;
#endif
}

std::size_t AudioWrite(const int16_t* frames, std::size_t count)
{
#if CFG_TUD_AUDIO
    // Nobody records: the frames go nowhere, as with a cable that is not plugged in.
    if (!g_recording.load(std::memory_order_relaxed)) return count;
    const std::size_t bytes = count * CARDENZA_USB_AUDIO_CHANNELS * CARDENZA_USB_AUDIO_BYTES;
    return tud_audio_write(frames, static_cast<uint16_t>(bytes)) / (CARDENZA_USB_AUDIO_CHANNELS * CARDENZA_USB_AUDIO_BYTES);
#else
    (void)frames;
    return count;
#endif
}

std::size_t AudioRead(int16_t* frames, std::size_t count)
{
#if CFG_TUD_AUDIO
    if (!g_playing.load(std::memory_order_relaxed)) return 0;
    constexpr std::size_t kFrameBytes = CARDENZA_USB_AUDIO_CHANNELS * CARDENZA_USB_AUDIO_BYTES;
    const std::size_t available = tud_audio_available() / kFrameBytes;
    // Adaptive playback: one frame dropped or repeated now and then keeps the FIFO where it should be.
    const int adjust = g_classes.feedback ? 0 : g_rateMatch.Decide(static_cast<int>(available), static_cast<int>(count));
    std::size_t take = std::min(count, available);
    if (adjust < 0 && take == count) --take;  // the last frame is played twice
    std::size_t got = take == 0 ? 0 : tud_audio_read(frames, static_cast<uint16_t>(take * kFrameBytes)) / kFrameBytes;
    if (got > 0) std::memcpy(g_lastFrame, frames + 2 * (got - 1), sizeof(g_lastFrame));
    if (adjust < 0 && got == count - 1)
    {
        std::memcpy(frames + 2 * got, g_lastFrame, sizeof(g_lastFrame));
        ++got;
    }
    if (adjust > 0 && tud_audio_available() >= kFrameBytes)
    {
        int16_t dropped[2];
        tud_audio_read(dropped, kFrameBytes);
    }
    return got;
#else
    (void)frames;
    (void)count;
    return 0;
#endif
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
#if CFG_TUD_AUDIO
    if (g_started.load() && g_classes.audio)
    {
        counters.fifoFromHost = tud_audio_available();
        if (tu_fifo_t* fifo = tud_audio_get_ep_in_ff()) counters.fifoToHost = tu_fifo_count(fifo);
    }
#endif
    return counters;
}

bool AudioInputActive() { return g_recording.load(std::memory_order_relaxed); }
bool AudioOutputActive() { return g_playing.load(std::memory_order_relaxed); }
bool HostMuted() { return g_hostMute.load(std::memory_order_relaxed); }
int16_t HostVolume() { return g_hostVolume.load(std::memory_order_relaxed); }

bool KeyboardReport(uint8_t modifiers, const uint8_t keys[6])
{
    if (!g_config.keyboard) return false;
    uint8_t report[8] = {modifiers, 0, keys[0], keys[1], keys[2], keys[3], keys[4], keys[5]};
    return detail::QueueInput(kReportKeyboard, report, sizeof(report), 0);
}

bool ConsumerReport(uint16_t usage)
{
    if (!g_config.consumer) return false;
    const uint8_t report[2] = {static_cast<uint8_t>(usage), static_cast<uint8_t>(usage >> 8)};
    return detail::QueueInput(kReportConsumer, report, sizeof(report), 0);
}

void SetAppReportHandler(ReportHandler handler) { g_appHandler.store(handler, std::memory_order_relaxed); }

bool SendAppReport(const uint8_t* data, std::size_t length)
{
    if (!g_started.load()) return false;
    switch (g_appWay.load(std::memory_order_relaxed))
    {
        case Way::Hid: return QueueReport(kReportApp, data, length);
        case Way::Serial: return SerialHex("app", data, std::min(length, kReportSize), 0);
        case Way::WebUsb: return QueueWebUsb(kReportApp, data, length);
        case Way::Sysex:
        {
            if (!Mounted() || !g_classes.midi) return false;
            uint8_t message[kReportSize];
            message[0] = kCommandApp;
            length = std::min(length, kReportSize - 1);
            std::memcpy(message + 1, data, length);
            return SendSysex(message, length + 1);
        }
        case Way::Text: return false;
    }
    return false;
}

namespace detail {

bool DevSendHid(const uint8_t report[kReportSize], uint32_t waitMs)
{
    for (uint32_t waited = 0;; ++waited)
    {
        if (!Mounted() || !g_classes.hid) return false;
        if (QueueReport(kReportSystem, report, kReportSize)) return true;
        if (waited >= waitMs) return false;
        vTaskDelay(1);
    }
}

bool DevSendSysex(const uint8_t* message, std::size_t length, uint32_t waitMs)
{
#if CFG_TUD_MIDI
    if (!Mounted() || !g_classes.midi) return false;
    return WriteSysex(message, length, waitMs);
#else
    (void)message;
    (void)length;
    (void)waitMs;
    return false;
#endif
}

bool DevSendSerial(const uint8_t report[kReportSize], uint32_t waitMs) { return SerialHex("sys", report, kReportSize, waitMs); }

bool DevSendWebUsb(const uint8_t report[kReportSize], uint32_t waitMs)
{
    // The page asks every few milliseconds: give it the time of a slow poll before the computer counts as gone.
    const uint32_t wait = waitMs == 0 ? 0 : std::max<uint32_t>(waitMs, 250);
    for (uint32_t waited = 0;; ++waited)
    {
        if (!Mounted() || !g_classes.webusb) return false;
        if (QueueWebUsb(kReportSystem, report, kReportSize)) return true;
        if (waited >= wait) return false;
        vTaskDelay(1);
    }
}

bool DevSendText(const uint8_t* bytes, std::size_t length, uint8_t flags, uint32_t waitMs)
{
#if CFG_TUD_CDC
    if (!g_serialAnswerOpen.exchange(true)) g_serialTextWritten = false;
    bool sent = true;
    if (flags & Dev::kFlagError)
    {
        char line[96];
        const int size = std::snprintf(line, sizeof(line), "%sERR %.*s\n", g_serialTextWritten ? "\n" : "", static_cast<int>(std::min<std::size_t>(length, 64)),
                                       reinterpret_cast<const char*>(bytes));
        sent = size > 0 && SerialAppend(line, std::min<std::size_t>(size, sizeof(line) - 1), waitMs);
        g_serialAnswerOpen.store(false);
        return sent;
    }
    if (length != 0)
    {
        sent = SerialAppend(reinterpret_cast<const char*>(bytes), length, waitMs);
        g_serialTextWritten = true;
    }
    if (sent && (flags & Dev::kFlagLast)) sent = SerialAppend("\n", 1, waitMs);
    if (!sent || (flags & Dev::kFlagLast)) g_serialAnswerOpen.store(false);
    return sent;
#else
    (void)bytes;
    (void)length;
    (void)flags;
    (void)waitMs;
    return false;
#endif
}

const char* Product() { return g_config.product != nullptr ? g_config.product : ""; }

Identity CurrentIdentity()
{
    Identity identity;
    identity.vendorId = g_config.vendorId;
    identity.productId = g_config.productId;
    if (g_serial[0] == '\0')
    {
        uint8_t mac[6] = {};
        esp_efuse_mac_get_default(mac);
        Logic::SerialNumber(mac, identity.serialNumber);
    }
    else
        std::memcpy(identity.serialNumber, g_serial, sizeof(identity.serialNumber));
    identity.classes = g_classes;
    identity.started = g_started.load();
    return identity;
}

bool CameraTransfer(const uint8_t* data, uint16_t length)
{
    if (!g_classes.camera || g_ep.cameraIn == 0 || !Mounted()) return false;
    if (!usbd_edpt_claim(0, g_ep.cameraIn)) return false;
    if (usbd_edpt_xfer(0, g_ep.cameraIn, const_cast<uint8_t*>(data), length, false)) return true;
    usbd_edpt_release(0, g_ep.cameraIn);
    return false;
}

#if CFG_TUD_HID
bool QueueInput(uint8_t reportId, const uint8_t* data, std::size_t length, uint32_t waitMs)
{
    if (length > sizeof(InputReport::data)) return false;
    for (uint32_t waited = 0;; ++waited)
    {
        if (!Mounted() || !g_classes.hid) return false;
        bool queued = false;
        portENTER_CRITICAL_SAFE(&g_lock);
        if (g_inputCount < kInputReports)
        {
            InputReport& input = g_inputs[(g_inputHead + g_inputCount) % kInputReports];
            input.id = reportId;
            input.length = static_cast<uint8_t>(length);
            std::memcpy(input.data, data, length);
            ++g_inputCount;
            queued = true;
        }
        portEXIT_CRITICAL_SAFE(&g_lock);
        if (queued) return true;
        if (waited >= waitMs) return false;
        vTaskDelay(1);
    }
}
#else
bool QueueInput(uint8_t, const uint8_t*, std::size_t, uint32_t) { return false; }
#endif

}  // namespace detail

void RestartIntoDownloadMode()
{
    ESP_LOGW(kTag, "restarting into the ROM download mode");
    LeaveHost();
    // The PHY goes back to USB-Serial-JTAG (this survives the restart) and the ROM stays in download mode.
    RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 0;
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

void RestartIntoLauncher()
{
    LeaveHost();
    // With the Launcher's restart ticket: its home screen even when "Resume after reset" is on.
    System::RestartIntoLauncher();
}

}  // namespace CardenzaHAL::USB

// ---- TinyUSB callbacks (C linkage, one definition each per firmware) ---------------------------------------------
using namespace CardenzaHAL::USB;

namespace {

// The WebUSB interface has no endpoints and no class: this driver takes it, so the configuration is whole.
// Its requests are vendor requests, which TinyUSB hands to tud_vendor_control_xfer_cb.
void WebUsbInit() {}
bool WebUsbDeinit() { return true; }
// A bus reset: answers the page has not read belong to the host that is gone.
void WebUsbReset(uint8_t)
{
#if CARDENZA_USB_BUILD_WEBUSB
    portENTER_CRITICAL_SAFE(&g_lock);
    g_webUsbCount = 0;
    portEXIT_CRITICAL_SAFE(&g_lock);
#endif
    CardenzaHAL::USB::detail::CameraOpened(false);
}
uint16_t WebUsbOpen(uint8_t rhport, tusb_desc_interface_t const* itf, uint16_t maxLength)
{
    if (itf->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC || itf->bNumEndpoints > 1 || itf->bInterfaceNumber != g_desc.itfWebUsb)
        return 0;
    uint16_t length = sizeof(tusb_desc_interface_t);
    if (maxLength < length) return 0;
    if (itf->bNumEndpoints == 1)
    {
        // The screen stream's bulk IN endpoint.
        const auto* endpoint = reinterpret_cast<tusb_desc_endpoint_t const*>(tu_desc_next(itf));
        if (maxLength < length + sizeof(tusb_desc_endpoint_t) || endpoint->bDescriptorType != TUSB_DESC_ENDPOINT) return 0;
        if (!usbd_edpt_open(rhport, endpoint)) return 0;
        length = static_cast<uint16_t>(length + sizeof(tusb_desc_endpoint_t));
        CardenzaHAL::USB::detail::CameraOpened(true);
    }
    return length;
}
bool WebUsbControl(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request)
{
    if (stage != CONTROL_STAGE_SETUP || request->bmRequestType_bit.type != TUSB_REQ_TYPE_STANDARD) return true;
    if (request->bRequest == TUSB_REQ_GET_INTERFACE)
    {
        static uint8_t alternate = 0;
        return tud_control_xfer(rhport, request, &alternate, 1);
    }
    if (request->bRequest == TUSB_REQ_SET_INTERFACE && request->wValue == 0) return tud_control_status(rhport, request);
    return false;
}
bool WebUsbTransfer(uint8_t, uint8_t endpoint, xfer_result_t result, uint32_t bytes)
{
    if (g_ep.cameraIn == 0 || endpoint != g_ep.cameraIn) return false;
    CardenzaHAL::USB::detail::CameraSent(result == XFER_RESULT_SUCCESS, bytes);
    return true;
}

const usbd_class_driver_t kWebUsbDriver = {
    "WEBUSB", WebUsbInit, WebUsbDeinit, WebUsbReset, WebUsbOpen, WebUsbControl, WebUsbTransfer, nullptr, nullptr,
};

}  // namespace

extern "C" {

usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t* count)
{
    *count = CARDENZA_USB_BUILD_WEBUSB ? 1 : 0;
    return CARDENZA_USB_BUILD_WEBUSB ? &kWebUsbDriver : nullptr;
}

uint8_t const* tud_descriptor_device_cb(void) { return reinterpret_cast<uint8_t const*>(&g_desc.device); }

uint8_t const* tud_descriptor_configuration_cb(uint8_t) { return g_desc.configuration; }

uint8_t const* tud_descriptor_bos_cb(void) { return g_desc.webusb ? g_desc.bos : nullptr; }

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
        case kStringManufacturer: text = kManufacturer; break;
        case kStringProduct: text = kBoardName; break;
        case kStringSerial: text = g_serial; break;
        case kStringAudio: text = "Cardenza Audio"; break;
        case kStringMidi: text = "Cardenza MIDI"; break;
        case kStringHid: text = "Cardenza HID"; break;
        case kStringSerialPort: text = "Cardenza Serial"; break;
        case kStringWebUsb: text = "Cardenza WebUSB"; break;
        case kStringDrive: text = "Cardenza Storage"; break;
        case kStringNetwork: text = "Cardenza Network"; break;
        case kStringNetworkMac: text = CardenzaHAL::USB::detail::NetworkMacString(); break;
        default: return nullptr;
    }
    const std::size_t length = std::min<std::size_t>(std::strlen(text), TU_ARRAY_SIZE(g_string) - 1);
    for (std::size_t i = 0; i < length; ++i) g_string[1 + i] = static_cast<uint8_t>(text[i]);
    g_string[0] = static_cast<uint16_t>((TUSB_DESC_STRING << 8) | (2 * length + 2));
    return g_string;
}

// Vendor requests: the WebUSB landing page and Microsoft OS 2.0 (device), the system channel (the WebUSB
// interface).
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request)
{
#if CARDENZA_USB_BUILD_WEBUSB
    if (!g_desc.webusb) return false;
    if (request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_DEVICE)
    {
        if (stage != CONTROL_STAGE_SETUP) return true;
        if (request->bRequest == kVendorWebUsb && request->wIndex == kWebUsbGetUrl)
        {
            static uint8_t url[3 + 64];
            const char* address = CONFIG_CARDENZA_HAL_USB_WEBUSB_URL;
            const std::size_t length = std::min<std::size_t>(std::strlen(address), sizeof(url) - 3);
            url[0] = static_cast<uint8_t>(3 + length);
            url[1] = 3;  // WEBUSB URL
            url[2] = 1;  // https://
            std::memcpy(url + 3, address, length);
            return tud_control_xfer(rhport, request, url, url[0]);
        }
        if (request->bRequest == kVendorMicrosoft && request->wIndex == kMsOs20Descriptor)
        {
            g_windowsHost.store(true, std::memory_order_relaxed);  // only Windows asks for it
            return tud_control_xfer(rhport, request, g_desc.msOs20, kMsOs20Length);
        }
        return false;
    }
    if (request->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE || TU_U16_LOW(request->wIndex) != g_desc.itfWebUsb) return false;
    // The screen stream's own requests (usb/camera.cpp).
    if (request->bRequest >= detail::kCameraRequestFirst && request->bRequest <= detail::kCameraRequestLast)
        return detail::CameraControl(rhport, stage, request);
    if (request->bRequest == kWebUsbSend && request->bmRequestType_bit.direction == TUSB_DIR_OUT)
    {
        const uint16_t length = std::min<uint16_t>(request->wLength, sizeof(g_webUsbIn));
        if (stage == CONTROL_STAGE_SETUP) return tud_control_xfer(rhport, request, g_webUsbIn, length);
        if (stage == CONTROL_STAGE_DATA && length > 0)
        {
            if (g_webUsbIn[0] == kReportSystem) HandleCommand(g_webUsbIn + 1, length - 1u, Way::WebUsb);
            else if (g_webUsbIn[0] == kReportApp) HandleApp(g_webUsbIn + 1, length - 1u, Way::WebUsb);
        }
        return true;
    }
    if (request->bRequest == kWebUsbReceive && request->bmRequestType_bit.direction == TUSB_DIR_IN)
    {
        if (stage != CONTROL_STAGE_SETUP) return true;
        bool any = false;
        portENTER_CRITICAL_SAFE(&g_lock);
        if (g_webUsbCount > 0)
        {
            std::memcpy(g_webUsbOut, g_webUsbQueue[g_webUsbHead], sizeof(g_webUsbOut));
            g_webUsbHead = (g_webUsbHead + 1) % kWebUsbReports;
            --g_webUsbCount;
            any = true;
        }
        portEXIT_CRITICAL_SAFE(&g_lock);
        g_hostTalked.store(true, std::memory_order_relaxed);
        return tud_control_xfer(rhport, request, g_webUsbOut, any ? std::min<uint16_t>(request->wLength, sizeof(g_webUsbOut)) : 0);
    }
    return false;
#else
    (void)rhport;
    (void)stage;
    (void)request;
    return false;
#endif
}

#if CFG_TUD_HID
uint8_t const* tud_hid_descriptor_report_cb(uint8_t) { return g_desc.hidReport; }

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
    if (report_id == kReportSystem) HandleCommand(buffer, bufsize, Way::Hid);
    else if (report_id == kReportApp) HandleApp(buffer, bufsize, Way::Hid);
    else if (report_id == kReportKeyboard && bufsize > 0) detail::HidKeyboardLeds(buffer[0]);  // its lights
}

void tud_hid_report_complete_cb(uint8_t, uint8_t const*, uint16_t) { PumpReports(); }
#endif

void tud_umount_cb(void)
{
    g_playing.store(false);
    g_recording.store(false);
    // The next host asks for the console itself, and its app reports pick their own way back.
    g_consoleOn.store(false);
    g_consoleWay.store(Way::Hid);
    g_appWay.store(Way::Hid);
    portENTER_CRITICAL_SAFE(&g_lock);
#if CARDENZA_USB_BUILD_WEBUSB
    g_webUsbCount = 0;
#endif
    g_pendingCount = 0;
    portEXIT_CRITICAL_SAFE(&g_lock);
    CardenzaHAL::USB::detail::DevHostGone();
    CardenzaHAL::USB::detail::HidInputReset();
    CardenzaHAL::USB::detail::DriveHostGone();
    CardenzaHAL::USB::detail::CameraOpened(false);
}

// A host configured the device: its streams start closed.
void tud_mount_cb(void)
{
    g_playing.store(false);
    g_recording.store(false);
}

// The host sleeps (or the cable went on a board with its own power: no VBUS sensing here, so it looks the same):
// nothing the computer held stays held, an answer under way stops.
void tud_suspend_cb(bool)
{
    CardenzaHAL::USB::detail::DevHostGone();
    CardenzaHAL::USB::detail::HidInputReset();
    CardenzaHAL::USB::detail::DriveSuspended(true);
}

void tud_resume_cb(void) { CardenzaHAL::USB::detail::DriveSuspended(false); }

#if CFG_TUD_CDC
// esptool's and Arduino's reset into the ROM's download mode (Logic::ResetDetector).
void tud_cdc_line_state_cb(uint8_t, bool dtr, bool rts)
{
    if (g_serialReset.Lines(dtr, rts))
    {
        ESP_LOGW("usb", "serial port: the reset into the download mode (DTR/RTS or 1200 baud)");
        g_restart.store(kCommandDownload, std::memory_order_relaxed);
    }
}

void tud_cdc_line_coding_cb(uint8_t, cdc_line_coding_t const* coding) { g_serialReset.Baud(coding->bit_rate); }
#endif

#if CFG_TUD_AUDIO
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
    // The audio function's interfaces: control, playback, recording (control, recording without the playback).
    const uint8_t playback = g_desc.audioPlayback ? g_desc.itfAudio + 1 : 0xFF;
    const uint8_t recording = g_desc.audioPlayback ? g_desc.itfAudio + 2 : g_desc.itfAudio + 1;
    if (itf == playback)
    {
        g_lastFromHostUs.store(esp_timer_get_time(), std::memory_order_relaxed);
        g_rateMatch.Reset();
        g_playing.store(alt != 0);
    }
    if (itf == recording)
    {
        if (alt != 0) tud_audio_clear_ep_in_ff();
        g_recording.store(alt != 0);
    }
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t, tusb_control_request_t const* request)
{
    const uint8_t itf = TU_U16_LOW(request->wIndex);
    const uint8_t playback = g_desc.audioPlayback ? g_desc.itfAudio + 1 : 0xFF;
    const uint8_t recording = g_desc.audioPlayback ? g_desc.itfAudio + 2 : g_desc.itfAudio + 1;
    if (itf == playback) g_playing.store(false);
    if (itf == recording) g_recording.store(false);
    return true;
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

#if CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP
void tud_audio_feedback_params_cb(uint8_t, uint8_t, audio_feedback_params_t* feedback)
{
    // The host's rate follows how full the playback FIFO is: the device's clock leads.
    feedback->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
    feedback->sample_freq = kSampleRate;
}
#endif
#endif  // CFG_TUD_AUDIO

// TinyUSB's audio driver allocates the feedback endpoint's TX FIFO for its 4 bytes: one word. The DWC2
// documentation gives 16 words as the smallest TX FIFO, so the feedback endpoint gets that (the FIFO
// RAM has room: 229 of 256 words in use); everything else is allocated as asked.
bool __real_usbd_edpt_iso_alloc(uint8_t rhport, uint8_t ep_addr, uint16_t largest_packet_size);
bool __wrap_usbd_edpt_iso_alloc(uint8_t rhport, uint8_t ep_addr, uint16_t largest_packet_size)
{
    if (g_ep.audioFeedback != 0 && ep_addr == g_ep.audioFeedback && largest_packet_size < 64) largest_packet_size = 64;
    return __real_usbd_edpt_iso_alloc(rhport, ep_addr, largest_packet_size);
}

// The feedback value. TinyUSB sends UAC2 feedback as 16.16 in 4 bytes at every speed, which is what
// Windows' driver wants; on full speed the USB specification says 10.14 in 3 bytes, and macOS reads it
// that way: 16.16 there means four times the rate, and macOS stops playing after its first 128 ms. A
// host that asks for a Microsoft OS descriptor (string 0xEE, or the MS OS 2.0 set with WebUSB) is taken for
// Windows and gets 16.16; every other host gets 10.14.
bool __real_usbd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes, bool is_isr);
bool __wrap_usbd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes, bool is_isr)
{
    if (g_ep.audioFeedback == 0 || ep_addr != g_ep.audioFeedback || total_bytes != 4 || g_windowsHost.load(std::memory_order_relaxed))
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

}  // extern "C"
