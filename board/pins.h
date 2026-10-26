#pragma once

#include <array>

namespace CardenzaHAL::Pins {

inline constexpr int kExternalI2cPort = 0;
inline constexpr int kExternalI2cScl = 1;
inline constexpr int kExternalI2cSda = 2;

inline constexpr int kDisplayMosi = 35;
inline constexpr int kDisplayMiso = -1;
inline constexpr int kDisplaySclk = 36;
inline constexpr int kDisplayDc = 34;
inline constexpr int kDisplayCs = 37;
inline constexpr int kDisplayReset = 33;
inline constexpr int kDisplayBacklight = 38;
inline constexpr int kDisplayLandscapeGapX = 40;
inline constexpr int kDisplayLandscapeGapY = 53;

inline constexpr int kSpeakerBck = 41;
inline constexpr int kSpeakerWs = 43;
inline constexpr int kSpeakerDataOut = 42;

inline constexpr int kMicDataIn = 46;
inline constexpr int kMicBck = -1;
inline constexpr int kMicWs = 43;

inline constexpr int kSdClk = 40;
inline constexpr int kSdCmd = 14;
inline constexpr int kSdDat0 = 39;
inline constexpr int kSdDat1 = 18;
inline constexpr int kSdDat2 = 17;
inline constexpr int kSdDat3 = 12;

inline constexpr int kIrTx = 44;
inline constexpr int kFuncButton = 0;

inline constexpr std::array<int, 3> kKeyboardOutputs = {8, 9, 11};
inline constexpr std::array<int, 7> kKeyboardInputs = {13, 15, 3, 4, 5, 6, 7};
// Override all three at build time for newer EN47 / CLK48 / DAT21 boards.
#ifndef CARDENZA_KEYBOARD_LED_EN_GPIO
#define CARDENZA_KEYBOARD_LED_EN_GPIO 21
#endif
#ifndef CARDENZA_KEYBOARD_LED_CLK_GPIO
#define CARDENZA_KEYBOARD_LED_CLK_GPIO 48
#endif
#ifndef CARDENZA_KEYBOARD_LED_DAT_GPIO
#define CARDENZA_KEYBOARD_LED_DAT_GPIO 47
#endif
inline constexpr std::array<int, 3> kKeyboardLedPins = {
    CARDENZA_KEYBOARD_LED_EN_GPIO, CARDENZA_KEYBOARD_LED_CLK_GPIO, CARDENZA_KEYBOARD_LED_DAT_GPIO
};

}  // namespace CardenzaHAL::Pins
