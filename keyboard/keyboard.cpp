#include "keyboard.h"
#include "internal/space_filter.h"

#include "internal/keyboard_driver.h"
#include "internal/flash_guard.h"
#include "internal/overlay.h"

namespace CardenzaHAL::Keyboard {

namespace {

KeyboardDriver keyboard;

}  // namespace

bool init()
{
    return keyboard.init();
}

Point2D_t getKey()
{
    return keyboard.getKey();
}

uint8_t getKeyNum(Point2D_t keyCoor)
{
    return keyboard.getKeyNum(keyCoor);
}

void updateKeyList()
{
    keyboard.updateKeyList();
}

std::vector<Point2D_t>& keyList()
{
    return keyboard.keyList();
}

bool isKeyPressing(int keyNum)
{
    return keyboard.isKeyPressing(keyNum);
}

bool isChanged()
{
    return keyboard.isChanged();
}

uint8_t isPressed()
{
    return keyboard.isPressed();
}

bool enableLED(bool enabled)
{
    return keyboard.enableLED(enabled);
}

void setLED(Point2D_t pos, uint8_t brightness)
{
    keyboard.setLED(pos, brightness);
}

void setLEDFrame(const LedFrame& levels)
{
    keyboard.setLEDFrame(levels);
}

void clearAllLEDs()
{
    keyboard.clearAllLEDs();
}

void setAllLEDs(uint8_t brightness)
{
    keyboard.setAllLEDs(brightness);
}

void setLEDGlobalBrightness(uint8_t level)
{
    keyboard.setLEDGlobalBrightness(level);
}

uint8_t ledGlobalBrightness()
{
    return keyboard.ledGlobalBrightness();
}

}  // namespace CardenzaHAL::Keyboard

namespace CardenzaHAL::Keyboard::detail {

void BeginOverlay(const LedFrame& lights)
{
    keyboard.beginOverlay(lights);
}

void EndOverlay()
{
    keyboard.endOverlay();
}

std::size_t OverlayKeys(Point2D_t* keys, std::size_t capacity)
{
    return keyboard.overlayKeys(keys, capacity);
}

bool KeyDown(int number)
{
    Point2D_t keys[16];
    const std::size_t count = keyboard.overlayKeys(keys, 16);
    for (std::size_t i = 0; i < count; ++i)
        if (keyboard.getKeyNum(keys[i]) == number) return true;
    return false;
}

void LightsOffForSleep()
{
    keyboard.lightsOffForSleep();
}

void HoldLightsDark(bool dark)
{
    keyboard.holdLightsDark(dark);
}

void SetOverlayFloor(bool on)
{
    keyboard.setOverlayFloor(on);
}

}  // namespace CardenzaHAL::Keyboard::detail
