#include "keyboard.h"

#include "internal/keyboard_driver.h"

namespace CardenzaHAL::Keyboard {

namespace {

KeyboardDriver keyboard;

}  // namespace

void init()
{
    keyboard.init();
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
