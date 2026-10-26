#pragma once

#include <vector>

#include "types.h"

namespace CardenzaHAL::Keyboard {

void init();
Point2D_t getKey();
uint8_t getKeyNum(Point2D_t keyCoor);
void updateKeyList();
std::vector<Point2D_t>& keyList();
bool isKeyPressing(int keyNum);
bool isChanged();
uint8_t isPressed();
bool enableLED(bool enabled);
// Per-key and global brightness use 16 levels (0 off, 15 maximum).
void setLED(Point2D_t pos, uint8_t brightness);
void clearAllLEDs();
void setAllLEDs(uint8_t brightness);
void setLEDGlobalBrightness(uint8_t level);
uint8_t ledGlobalBrightness();

}  // namespace CardenzaHAL::Keyboard
