// CardenzaHAL::USB on the web target. There is no USB cable: the page is the host.
//
// - MIDI goes to and comes from the computer's MIDI ports through Web MIDI when the page may use it
//   (a page in a sandboxed frame usually may not: the packets then go nowhere, like a host that is
//   not listening). System SysEx (F0 23 <command> ... F7) is answered here as the device answers it:
//   identify, reboot and the restarts into the Launcher or the ROM download mode (both restart the
//   simulator), and the app channel (F0 23 7F ...).
// - The app channel (SetAppReportHandler / SendAppReport) also reaches the page's script and its
//   parent (postMessage), so a web page can talk to the firmware as a host tool would.
// - Audio: the host neither records nor plays (AudioWrite takes the frames, AudioRead gives none).
// - HID keyboard and consumer reports have no host: they report false.
// Handlers run in a "usb" task (priority 8, core 0) as on the device.

#include "usb/usb.h"
#include "usb/hid.h"
#include "usb/hid_keys.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "web/src/kernel/kernel.h"
#include "web/src/platform.h"
#include "web_usb.h"
#include "usb/internal/start.h"

namespace CardenzaHAL::USB {

namespace {

namespace k = cardenza_web::kernel;
constexpr char kTag[] = "usb";

Config g_config;
bool g_started = false;
TaskHandle_t g_task = nullptr;
ReportHandler g_appHandler = nullptr;
bool g_appSysex = false;

constexpr std::size_t kQueue = 64;
uint8_t g_midiIn[kQueue][4];
std::size_t g_midiHead = 0, g_midiCount = 0;       // for MidiRead
uint8_t g_rawIn[kQueue][4];
std::size_t g_rawHead = 0, g_rawCount = 0;         // from the page, for the USB task
uint8_t g_apps[4][kReportSize];
std::size_t g_appLengths[4];
std::size_t g_appHead = 0, g_appCount = 0;
uint8_t g_sysex[kReportSize];
std::size_t g_sysexLength = 0;
bool g_sysexOurs = false, g_sysexActive = false;

void SendSysex(const uint8_t* data, std::size_t length)
{
    uint8_t message[kReportSize + 3];
    std::size_t size = 0;
    message[size++] = 0xF0;
    message[size++] = 0x24;
    for (std::size_t index = 0; index < length && size < sizeof(message) - 1; ++index) message[size++] = data[index] & 0x7F;
    message[size++] = 0xF7;
    for (std::size_t at = 0; at < size; at += 3)
    {
        const std::size_t left = size - at;
        uint8_t packet[4] = {0x04, 0, 0, 0};
        if (left <= 3) packet[0] = static_cast<uint8_t>(0x04 + left);
        for (std::size_t index = 0; index < 3 && index < left; ++index) packet[1 + index] = message[at + index];
        cardenza_web::platform::MidiOut(packet);
    }
}

void HandleCommand(const uint8_t* data, std::size_t length)
{
    if (length == 0) return;
    const uint8_t command = data[0];
    switch (command)
    {
        case kCommandIdentify:
        {
            uint8_t answer[kReportSize] = {static_cast<uint8_t>(command | 0x80), kProtocolVersion};
            std::strncpy(reinterpret_cast<char*>(answer + 2), g_config.product, kReportSize - 3);
            SendSysex(answer, 2 + std::strlen(reinterpret_cast<char*>(answer + 2)));
            return;
        }
        case kCommandBootloader:
        case kCommandReboot:
        case kCommandDownload:
        {
            const uint8_t answer[1] = {static_cast<uint8_t>(command | 0x80)};
            SendSysex(answer, 1);
            vTaskDelay(pdMS_TO_TICKS(20));
            if (command == kCommandDownload) RestartIntoDownloadMode();
            if (command == kCommandBootloader) RestartIntoLauncher();
            esp_restart();
        }
        case kCommandApp:
            g_appSysex = true;
            if (g_appHandler != nullptr) g_appHandler(data + 1, length - 1);
            return;
        default:
            return;  // the console stays the page's
    }
}

void Deliver(const uint8_t packet[4])
{
    if (g_midiCount == kQueue) return;  // full: the newest goes
    std::memcpy(g_midiIn[(g_midiHead + g_midiCount) % kQueue], packet, 4);
    ++g_midiCount;
}

// The device's SysEx collector: our system SysEx is answered, anything else goes to MidiRead.
void PumpMidi()
{
    while (g_rawCount > 0)
    {
        uint8_t packet[4];
        std::memcpy(packet, g_rawIn[g_rawHead], 4);
        g_rawHead = (g_rawHead + 1) % kQueue;
        --g_rawCount;
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
                if (g_sysexOurs) HandleCommand(g_sysex, g_sysexLength);
                g_sysexActive = false;
            }
            if (g_sysexOurs) continue;
        }
        Deliver(packet);
    }
}

void UsbTask(void*)
{
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        PumpMidi();
        while (g_appCount > 0)
        {
            uint8_t report[kReportSize];
            const std::size_t length = g_appLengths[g_appHead];
            std::memcpy(report, g_apps[g_appHead], length);
            g_appHead = (g_appHead + 1) % 4;
            --g_appCount;
            g_appSysex = false;  // the page's own channel answers the same way
            if (g_appHandler != nullptr) g_appHandler(report, length);
        }
    }
}

void Wake(void*)
{
    if (g_task == nullptr) return;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(g_task, &woken);
}

}  // namespace

bool Init(const Config& config)
{
    if (g_started) return false;
    g_config = config;
    if (xTaskCreatePinnedToCore(UsbTask, "usb", 4096, nullptr, 8, &g_task, 0) != pdPASS)
    {
        ESP_LOGE(kTag, "USB task failed");
        return false;
    }
    g_started = true;
    cardenza_web::platform::UsbStarted(g_config.product);
    ESP_LOGI(kTag, "started %04x:%04x audio=%d midi=%d keyboard=%d consumer=%d (web: Web MIDI and the page)", g_config.vendorId,
             g_config.productId, g_config.audio, g_config.midi, g_config.keyboard, g_config.consumer);
    return true;
}

bool Started() { return g_started; }
bool Mounted() { return g_started; }

bool MidiRead(uint8_t packet[4])
{
    k::PreemptionPoint();
    if (g_midiCount == 0) return false;
    std::memcpy(packet, g_midiIn[g_midiHead], 4);
    g_midiHead = (g_midiHead + 1) % kQueue;
    --g_midiCount;
    return true;
}

bool MidiWrite(const uint8_t packet[4])
{
    if (!Mounted() || !g_config.midi) return false;
    cardenza_web::platform::MidiOut(packet);
    return true;
}

std::size_t AudioWrite(const int16_t*, std::size_t count) { return count; }
std::size_t AudioRead(int16_t*, std::size_t) { return 0; }
bool AudioInputActive() { return false; }
bool AudioOutputActive() { return false; }
AudioCounters AudioStats() { return AudioCounters{}; }
bool HostMuted() { return false; }
int16_t HostVolume() { return 0; }
bool KeyboardReport(uint8_t, const uint8_t[6]) { return false; }
bool ConsumerReport(uint16_t) { return false; }
void SetAppReportHandler(ReportHandler handler) { g_appHandler = handler; }

bool SendAppReport(const uint8_t* data, std::size_t length)
{
    if (!g_started) return false;
    length = length < kReportSize ? length : kReportSize;
    if (g_appSysex)
    {
        uint8_t message[kReportSize];
        message[0] = kCommandApp;
        const std::size_t body = length < kReportSize - 1 ? length : kReportSize - 1;
        std::memcpy(message + 1, data, body);
        SendSysex(message, body + 1);
    }
    cardenza_web::platform::AppReport(data, length);
    return true;
}

// CardenzaHAL::Init's own start: the page has no computer to be found by, and Web MIDI starts (with the
// browser's permission prompt) only with the firmware's own USB::Init, as before.
namespace detail {
void AutoStart() {}
}  // namespace detail

// The classes cannot change the page's MIDI; the configuration is kept for CurrentConfig.
bool Reconfigure(const Config& config)
{
    if (!g_started) return false;
    g_config = config;
    return true;
}

Config CurrentConfig() { return g_config; }

Presented Classes()
{
    Presented presented;
    presented.midi = g_started && g_config.midi;
    return presented;
}

// HID inputs (usb/hid.h): the page is no computer's keyboard or gamepad. Nothing is ever on its way (false, as
// KeyboardReport); the gamepad's state is kept as on the board.
namespace Hid {
namespace {
Gamepad g_gamepad;
}  // namespace
bool Ready() { return false; }
bool Press(uint8_t, uint32_t) { return false; }
bool Release(uint8_t, uint32_t) { return true; }
bool ReleaseAll(uint32_t) { return true; }
bool Tap(uint8_t, uint32_t) { return false; }
bool Type(const char*, uint32_t) { return false; }
uint8_t KeyboardLeds() { return 0; }
bool MediaPress(uint16_t, uint32_t) { return false; }
bool MediaRelease(uint32_t) { return false; }
bool Media(uint16_t, uint32_t) { return false; }
bool SetGamepad(const Gamepad& state, uint32_t)
{
    g_gamepad = state;
    return false;
}
Gamepad GamepadState() { return g_gamepad; }
uint8_t HatFrom(bool up, bool down, bool left, bool right) { return HidKeys::HatFrom(up, down, left, right); }
}  // namespace Hid

void RestartIntoDownloadMode()
{
    ESP_LOGW(kTag, "restarting into the ROM download mode (the simulator restarts the firmware)");
    cardenza_web::platform::Restart(ESP_RST_SW);
}

void RestartIntoLauncher()
{
    ESP_LOGW(kTag, "restarting into the Launcher (the simulator restarts the firmware)");
    cardenza_web::platform::Restart(ESP_RST_SW);
}

}  // namespace CardenzaHAL::USB

namespace cardenza_web::usb {

void MidiFromHost(const uint8_t packet[4])
{
    using namespace CardenzaHAL::USB;
    if (!g_started || g_rawCount == kQueue) return;
    std::memcpy(g_rawIn[(g_rawHead + g_rawCount) % kQueue], packet, 4);
    ++g_rawCount;
    kernel::RunIsr(Wake, nullptr);
}

void AppFromHost(const uint8_t* data, std::size_t length)
{
    using namespace CardenzaHAL::USB;
    if (!g_started || g_appCount == 4) return;
    const std::size_t slot = (g_appHead + g_appCount) % 4;
    g_appLengths[slot] = length < kReportSize ? length : kReportSize;
    std::memcpy(g_apps[slot], data, g_appLengths[slot]);
    ++g_appCount;
    kernel::RunIsr(Wake, nullptr);
}

}  // namespace cardenza_web::usb
