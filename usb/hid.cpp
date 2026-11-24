// HID inputs for a firmware (usb/hid.h): the keys, the media key and the gamepad held, sent as report IDs 1, 3
// and 4 of the HID interface, which they share (and its one IN endpoint) with the system channel (report 0xCB)
// and the firmware's raw reports (0xFF); usb/usb.cpp sends every report in order (detail::QueueInput).
//
// The state is the truth: a report that cannot go now (no room, no computer) is marked and sent by the USB task
// as soon as it can (HidInputPump), a computer that comes back gets the state as it is, and when the computer
// goes or the classes change (HidInputReset) nothing is held any more.
#include "usb/hid.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "usb/hid_keys.h"
#include "usb/internal/dev.h"
#include "usb/usb.h"
#include "tusb.h"

namespace CardenzaHAL::USB {

namespace {

enum : uint8_t { kReportKeyboard = 1, kReportConsumer = 3, kReportGamepad = 4 };  // as in Matrix OS
enum : uint8_t { kDirtyKeyboard = 1, kDirtyConsumer = 2, kDirtyGamepad = 4 };

static_assert(sizeof(hid_gamepad_report_t) == 11, "TinyUSB's gamepad report");

// The state, under g_lock (short critical sections: the USB task reads it too).
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
HidKeys::KeySet g_keys;
uint16_t g_consumer = 0;
Hid::Gamepad g_gamepad;
uint8_t g_dirty = 0;            // what the computer has not got yet
std::atomic<uint8_t> g_leds{0};
bool g_wasReady = false;        // USB task only

// One sender at a time (the firmware's tasks; the USB task only tries it), so reports leave in the order the
// state changed.
SemaphoreHandle_t Sender()
{
    static StaticSemaphore_t storage;
    static SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&storage);
    return mutex;
}

struct Sending {
    Sending() { xSemaphoreTake(Sender(), portMAX_DELAY); }
    ~Sending() { xSemaphoreGive(Sender()); }
};

bool HidPresented() { return Classes().hid; }
bool HostReady() { return Mounted() && HidPresented(); }

// Which inputs the device's report descriptor has.
uint8_t Kinds()
{
    if (!HidPresented()) return 0;
    const Config config = CurrentConfig();
    return static_cast<uint8_t>((config.keyboard ? kDirtyKeyboard : 0) | (config.consumer ? kDirtyConsumer : 0) |
                                (config.gamepad ? kDirtyGamepad : 0));
}

void Mark(uint8_t kind, bool sent)
{
    portENTER_CRITICAL(&g_lock);
    if (sent) g_dirty = static_cast<uint8_t>(g_dirty & ~kind);
    else g_dirty = static_cast<uint8_t>(g_dirty | kind);
    portEXIT_CRITICAL(&g_lock);
}

// The current state of one input to the computer (with the sender held). False: marked, it goes later.
bool Send(uint8_t kind, uint32_t waitMs)
{
    uint8_t report[11] = {};
    std::size_t length = 0;
    uint8_t id = 0;
    portENTER_CRITICAL(&g_lock);
    if (kind == kDirtyKeyboard)
    {
        g_keys.Report(report);
        length = 8;
        id = kReportKeyboard;
    }
    else if (kind == kDirtyConsumer)
    {
        report[0] = static_cast<uint8_t>(g_consumer);
        report[1] = static_cast<uint8_t>(g_consumer >> 8);
        length = 2;
        id = kReportConsumer;
    }
    else
    {
        hid_gamepad_report_t gamepad = {g_gamepad.x, g_gamepad.y, g_gamepad.z, g_gamepad.rz, g_gamepad.rx, g_gamepad.ry, g_gamepad.hat, g_gamepad.buttons};
        std::memcpy(report, &gamepad, sizeof(gamepad));
        length = sizeof(gamepad);
        id = kReportGamepad;
    }
    portEXIT_CRITICAL(&g_lock);
    const bool sent = (Kinds() & kind) && HostReady() && detail::QueueInput(id, report, length, waitMs);
    Mark(kind, sent);
    return sent;
}

// A report of the keyboard other than the state (Type's strokes), waiting until `deadline` (µs) for room.
bool SendKeyboardRaw(const uint8_t report[8], int64_t deadline)
{
    const int64_t left = deadline - esp_timer_get_time();
    if (left <= 0 || !HostReady()) return false;
    return detail::QueueInput(kReportKeyboard, report, 8, static_cast<uint32_t>(left / 1000) + 1);
}

}  // namespace

namespace Hid {

bool Ready() { return HostReady(); }

bool Press(uint8_t usage, uint32_t waitMs)
{
    Sending sending;
    portENTER_CRITICAL(&g_lock);
    const HidKeys::KeySet::Result result = g_keys.Press(usage);
    portEXIT_CRITICAL(&g_lock);
    if (result == HidKeys::KeySet::Result::Full || result == HidKeys::KeySet::Result::Invalid) return false;
    if (result == HidKeys::KeySet::Result::Unchanged)
    {
        portENTER_CRITICAL(&g_lock);
        const bool waiting = g_dirty & kDirtyKeyboard;
        portEXIT_CRITICAL(&g_lock);
        if (!waiting) return HostReady() && (Kinds() & kDirtyKeyboard);
    }
    return Send(kDirtyKeyboard, waitMs);
}

bool Release(uint8_t usage, uint32_t waitMs)
{
    Sending sending;
    portENTER_CRITICAL(&g_lock);
    const HidKeys::KeySet::Result result = g_keys.Release(usage);
    const bool waiting = g_dirty & kDirtyKeyboard;
    portEXIT_CRITICAL(&g_lock);
    if (result == HidKeys::KeySet::Result::Invalid) return false;
    if (result == HidKeys::KeySet::Result::Unchanged && !waiting) return true;
    return Send(kDirtyKeyboard, waitMs);
}

bool ReleaseAll(uint32_t waitMs)
{
    Sending sending;
    portENTER_CRITICAL(&g_lock);
    const bool changed = !g_keys.Empty() || (g_dirty & kDirtyKeyboard);
    g_keys.Clear();
    portEXIT_CRITICAL(&g_lock);
    return changed ? Send(kDirtyKeyboard, waitMs) : true;
}

bool Tap(uint8_t usage, uint32_t waitMs)
{
    const bool pressed = Press(usage, waitMs);
    // Released whatever happened: never left down.
    const bool released = Release(usage, waitMs);
    return pressed && released;
}

bool Type(const char* text, uint32_t timeoutMs)
{
    if (text == nullptr || !HostReady() || !(Kinds() & kDirtyKeyboard)) return false;
    Sending sending;
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeoutMs) * 1000;
    uint8_t held[8];
    portENTER_CRITICAL(&g_lock);
    g_keys.Report(held);
    const bool anyHeld = !g_keys.Empty();
    portEXIT_CRITICAL(&g_lock);
    static constexpr uint8_t kUp[8] = {};
    bool whole = true;
    bool stuck = false;  // a key may be down at the computer: the state must follow
    if (anyHeld && !SendKeyboardRaw(kUp, deadline)) whole = false, stuck = true;
    for (const char* at = text; whole && *at != '\0'; ++at)
    {
        if (*at == '\r') continue;
        const HidKeys::Stroke stroke = HidKeys::FromAscii(*at);
        if (stroke.usage == 0)
        {
            whole = false;  // no key types it: skipped
            continue;
        }
        const uint8_t down[8] = {static_cast<uint8_t>(stroke.shift ? HidKeys::kModifierLeftShift : 0), 0, stroke.usage, 0, 0, 0, 0, 0};
        if (!SendKeyboardRaw(down, deadline))
        {
            whole = false;
            stuck = true;
            break;
        }
        if (!SendKeyboardRaw(kUp, deadline))
        {
            whole = false;
            stuck = true;
            break;
        }
    }
    // The keys held come back (and, after a stroke that did not finish, the computer gets the state as it is).
    if (anyHeld || stuck) Send(kDirtyKeyboard, static_cast<uint32_t>(std::max<int64_t>(0, (deadline - esp_timer_get_time()) / 1000)) + 20);
    return whole;
}

uint8_t KeyboardLeds() { return g_leds.load(std::memory_order_relaxed); }

bool MediaPress(uint16_t usage, uint32_t waitMs)
{
    Sending sending;
    portENTER_CRITICAL(&g_lock);
    const bool changed = g_consumer != usage || (g_dirty & kDirtyConsumer);
    g_consumer = usage;
    portEXIT_CRITICAL(&g_lock);
    return changed ? Send(kDirtyConsumer, waitMs) : HostReady() && (Kinds() & kDirtyConsumer);
}

bool MediaRelease(uint32_t waitMs) { return MediaPress(0, waitMs); }

bool Media(uint16_t usage, uint32_t waitMs)
{
    const bool pressed = MediaPress(usage, waitMs);
    const bool released = MediaRelease(waitMs);
    return pressed && released;
}

bool SetGamepad(const Gamepad& state, uint32_t waitMs)
{
    Sending sending;
    portENTER_CRITICAL(&g_lock);
    const bool changed = std::memcmp(&g_gamepad, &state, sizeof(state)) != 0 || (g_dirty & kDirtyGamepad);
    g_gamepad = state;
    portEXIT_CRITICAL(&g_lock);
    return changed ? Send(kDirtyGamepad, waitMs) : HostReady() && (Kinds() & kDirtyGamepad);
}

Gamepad GamepadState()
{
    portENTER_CRITICAL(&g_lock);
    const Gamepad state = g_gamepad;
    portEXIT_CRITICAL(&g_lock);
    return state;
}

uint8_t HatFrom(bool up, bool down, bool left, bool right) { return HidKeys::HatFrom(up, down, left, right); }

}  // namespace Hid

namespace detail {

#if CFG_TUD_HID
std::size_t HidInputReportDescriptor(uint8_t* out, std::size_t capacity, bool keyboard, bool consumer, bool gamepad)
{
    std::size_t length = 0;
    const auto add = [&](const uint8_t* block, std::size_t size) {
        if (length + size > capacity) return;
        std::memcpy(out + length, block, size);
        length += size;
    };
    if (keyboard)
    {
        const uint8_t block[] = {TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(kReportKeyboard))};
        add(block, sizeof(block));
    }
    if (consumer)
    {
        const uint8_t block[] = {TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(kReportConsumer))};
        add(block, sizeof(block));
    }
    if (gamepad)
    {
        const uint8_t block[] = {TUD_HID_REPORT_DESC_GAMEPAD(HID_REPORT_ID(kReportGamepad))};
        add(block, sizeof(block));
    }
    return length;
}
#else
std::size_t HidInputReportDescriptor(uint8_t*, std::size_t, bool, bool, bool) { return 0; }
#endif

void HidInputReset()
{
    portENTER_CRITICAL(&g_lock);
    g_keys.Clear();
    g_consumer = 0;
    g_gamepad = Hid::Gamepad{};
    g_dirty = 0;
    portEXIT_CRITICAL(&g_lock);
    g_leds.store(0, std::memory_order_relaxed);
}

void HidInputPump()
{
    const bool ready = HostReady();
    const uint8_t kinds = ready ? Kinds() : 0;
    // A computer that comes (plugged in, awake again): the state as it is, the empty one too.
    if (ready && !g_wasReady)
    {
        portENTER_CRITICAL(&g_lock);
        g_dirty = static_cast<uint8_t>(g_dirty | kinds);
        portEXIT_CRITICAL(&g_lock);
    }
    g_wasReady = ready;
    if (!ready) return;
    portENTER_CRITICAL(&g_lock);
    const uint8_t dirty = static_cast<uint8_t>(g_dirty & kinds);
    portEXIT_CRITICAL(&g_lock);
    if (dirty == 0 || xSemaphoreTake(Sender(), 0) != pdTRUE) return;  // a firmware's call is sending: it goes on
    for (const uint8_t kind : {kDirtyKeyboard, kDirtyConsumer, kDirtyGamepad})
        if (dirty & kind) Send(kind, 0);
    xSemaphoreGive(Sender());
}

void HidKeyboardLeds(uint8_t leds) { g_leds.store(leds, std::memory_order_relaxed); }

}  // namespace detail

}  // namespace CardenzaHAL::USB
