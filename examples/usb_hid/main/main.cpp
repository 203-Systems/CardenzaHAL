// USB HID inputs (usb/hid.h): the Cardenza as the computer's keyboard, media keys and gamepad, beside the HAL's
// system channel (which keeps answering on the same HID interface).
//
// - ok types a line ("Hello from Cardenza!" and Enter).
// - 1 play/pause, 2 previous, 3 next, 4 mute, 5 volume down, 6 volume up (media keys).
// - The arrows (; . , /) are the gamepad's hat, 7 8 9 0 its buttons 1 to 4.
// - Every other key is held as the computer's key while you hold it: the letters, esc, tab, del (backspace),
//   space, shift, ctrl, alt and opt (Command on a Mac). Six keys and the modifiers at a time.
// - The screen: the 4 x 14 keys (held ones filled), a green square while a computer has the HID interface,
//   a yellow one while the computer's caps lock is on, and the hat's direction.
//
// The keys are the caps' legends (keyboard/legends.h). macOS asks once which kind of keyboard this is.

#include <array>
#include <cstdint>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "cardenzaHAL.h"
#include "keyboard/led_light.h"
#include "keyboard/legends.h"
#include "usb/hid.h"
#include "usb/hid_keys.h"
#include "usb/usb.h"

namespace {

namespace Display = CardenzaHAL::Display;
namespace Hid = CardenzaHAL::USB::Hid;
namespace Keyboard = CardenzaHAL::Keyboard;
namespace USB = CardenzaHAL::USB;

constexpr char kTag[] = "usb_hid";
constexpr int kColumns = 14;
constexpr int kRows = 4;
constexpr int kCellWidth = 16;
constexpr int kCellHeight = 22;
constexpr int kGridX = (Display::WIDTH - kColumns * kCellWidth) / 2;
constexpr int kGridY = 36;

constexpr uint16_t Rgb(int r, int g, int b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
constexpr uint16_t kBlack = Rgb(0, 0, 0);
constexpr uint16_t kInk = Rgb(232, 232, 224);
constexpr uint16_t kFaint = Rgb(58, 58, 54);
constexpr uint16_t kGreen = Rgb(72, 255, 120);
constexpr uint16_t kYellow = Rgb(255, 208, 48);

alignas(4) uint16_t g_frame[Display::WIDTH * Display::HEIGHT];

void Fill(int x, int y, int width, int height, uint16_t colour)
{
    for (int row = y; row < y + height; ++row)
        for (int column = x; column < x + width; ++column)
            if (row >= 0 && row < Display::HEIGHT && column >= 0 && column < Display::WIDTH) g_frame[row * Display::WIDTH + column] = colour;
}

void Outline(int x, int y, int width, int height, uint16_t colour)
{
    Fill(x, y, width, 1, colour);
    Fill(x, y + height - 1, width, 1, colour);
    Fill(x, y, 1, height, colour);
    Fill(x + width - 1, y, 1, height, colour);
}

// What a key does.
enum class Role : uint8_t { None, Type, Media, Hat, Button, Key };
struct Action {
    Role role = Role::None;
    uint16_t value = 0;  // the media usage, the button, the hat's direction, the keyboard usage
};

Action ActionFor(uint8_t number)
{
    using namespace CardenzaHAL::Keyboard;
    switch (number)
    {
        case kKeyOk: return {Role::Type, 0};
        case 2: return {Role::Media, Hid::kMediaPlayPause};
        case 3: return {Role::Media, Hid::kMediaPrevious};
        case 4: return {Role::Media, Hid::kMediaNext};
        case 5: return {Role::Media, Hid::kMediaMute};
        case 6: return {Role::Media, Hid::kMediaVolumeDown};
        case 7: return {Role::Media, Hid::kMediaVolumeUp};
        case kKeyUp: return {Role::Hat, 0};
        case kKeyDown: return {Role::Hat, 1};
        case kKeyLeft: return {Role::Hat, 2};
        case kKeyRight: return {Role::Hat, 3};
        case 8: return {Role::Button, 0};
        case 9: return {Role::Button, 1};
        case 10: return {Role::Button, 2};
        case 11: return {Role::Button, 3};
        case kKeyEsc: return {Role::Key, Hid::kKeyEscape};
        case kKeyTab: return {Role::Key, Hid::kKeyTab};
        case kKeyDel: return {Role::Key, Hid::kKeyBackspace};
        case kKeySpace: return {Role::Key, Hid::kKeySpace};
        case kKeyShift: return {Role::Key, Hid::kKeyLeftShift};
        case kKeyCtrl: return {Role::Key, Hid::kKeyLeftCtrl};
        case kKeyAlt: return {Role::Key, Hid::kKeyLeftAlt};
        case kKeyOpt: return {Role::Key, Hid::kKeyLeftGui};
        case kKeyFn: return {};
        default: break;
    }
    if (number == 0 || number > KEY_COUNT) return {};
    // A key that types a character: the computer's key for it (without shift: shift is its own key here).
    const char character = kKeyLegends[number - 1].character;
    const auto stroke = CardenzaHAL::USB::HidKeys::FromAscii(character);
    if (character == 0 || stroke.usage == 0) return {};
    return {Role::Key, stroke.usage};
}

void Draw(const std::vector<Keyboard::Point2D_t>& keys, uint8_t hat)
{
    Fill(0, 0, Display::WIDTH, Display::HEIGHT, kBlack);
    Fill(8, 10, 14, 14, Hid::Ready() ? kGreen : kFaint);
    Fill(28, 10, 14, 14, (Hid::KeyboardLeds() & Hid::kLedCapsLock) ? kYellow : kFaint);
    // The hat: a dot around a centre.
    static constexpr int kDx[9] = {0, 0, 1, 1, 1, 0, -1, -1, -1};
    static constexpr int kDy[9] = {0, -1, -1, 0, 1, 1, 1, 0, -1};
    Outline(Display::WIDTH - 34, 4, 26, 26, kFaint);
    Fill(Display::WIDTH - 24 + 8 * kDx[hat < 9 ? hat : 0], 14 + 8 * kDy[hat < 9 ? hat : 0], 6, 6, kInk);
    std::array<bool, kRows * kColumns> held{};
    for (const auto& key : keys)
        if (key.y < kRows && key.x < kColumns) held[key.y * kColumns + key.x] = true;
    for (int y = 0; y < kRows; ++y)
        for (int x = 0; x < kColumns; ++x)
        {
            const int left = kGridX + x * kCellWidth;
            const int top = kGridY + y * kCellHeight;
            if (held[y * kColumns + x]) Fill(left + 1, top + 1, kCellWidth - 2, kCellHeight - 2, kInk);
            else Outline(left + 1, top + 1, kCellWidth - 2, kCellHeight - 2, kFaint);
        }
}

}  // namespace

extern "C" void app_main()
{
    if (!CardenzaHAL::Init())
    {
        const auto& report = CardenzaHAL::LastInitReport();
        ESP_LOGE(kTag, "HAL init failed: system=%d keyboard=%d display=%d", report.system, report.keyboard, report.display);
    }
    // CardenzaHAL::Init started USB with the defaults; this firmware adds the gamepad (right after Init: the
    // computer sees only this device).
    USB::Config usb;
    usb.product = "usb_hid";
    usb.gamepad = true;
    USB::Init(usb);
    Keyboard::enableLED(true);
    ESP_LOGI(kTag, "ready: ok types a line, 1-6 media keys, arrows and 7-0 the gamepad, other keys are keys");

    std::array<bool, Keyboard::KEY_COUNT + 1> before{};
    Hid::Gamepad pad;
    bool hat[4] = {};
    TickType_t wake = xTaskGetTickCount();
    for (;;)
    {
        Keyboard::updateKeyList();
        const auto& keys = Keyboard::keyList();
        std::array<bool, Keyboard::KEY_COUNT + 1> now{};
        for (const auto& key : keys) now[Keyboard::getKeyNum(key)] = true;
        bool padChanged = false;
        for (uint8_t number = 1; number <= Keyboard::KEY_COUNT; ++number)
        {
            if (now[number] == before[number]) continue;
            const bool down = now[number];
            const Action action = ActionFor(number);
            switch (action.role)
            {
                case Role::Type:
                    if (down) ESP_LOGI(kTag, "type: %s", Hid::Type("Hello from Cardenza!\n") ? "sent" : "not sent");
                    break;
                case Role::Media:
                    if (down) ESP_LOGI(kTag, "media 0x%02x: %s", action.value, Hid::Media(action.value) ? "sent" : "not sent");
                    break;
                case Role::Hat:
                    hat[action.value] = down;
                    padChanged = true;
                    break;
                case Role::Button:
                    if (down) pad.buttons |= 1u << action.value;
                    else pad.buttons &= ~(1u << action.value);
                    padChanged = true;
                    break;
                case Role::Key:
                    if (down) Hid::Press(static_cast<uint8_t>(action.value));
                    else Hid::Release(static_cast<uint8_t>(action.value));
                    break;
                case Role::None: break;
            }
        }
        if (padChanged)
        {
            pad.hat = Hid::HatFrom(hat[0], hat[1], hat[2], hat[3]);
            Hid::SetGamepad(pad);
        }
        before = now;

        Keyboard::LedFrame leds{};
        leds.fill(Keyboard::Light::PerceivedToLevel(4));
        for (const auto& key : keys) leds[key.y * Keyboard::LED_COLUMNS + key.x] = Keyboard::Light::PerceivedToLevel(15);
        Keyboard::setLEDFrame(leds);

        Draw(keys, pad.hat);
        Display::PresentChanged(g_frame);
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(10));
    }
}
