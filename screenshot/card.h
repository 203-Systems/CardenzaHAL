#pragma once

#include <cstdint>

#include "screenshot/logic.h"

// The screenshot's effect on the screen (screenshot/logic.h, Logic::Effect): the flash and the card, drawn with the
// 203 Launcher's own code as the system menu is (system/menu/launcher/). Pure: no ESP-IDF, so the host tests draw
// it (tests/screenshot_test.cpp) and the Launcher's copy draws the same pixels.
namespace CardenzaHAL::Screenshot::Card {

    // What the card says (its second line: the file's name, or "Not saved").
    enum class Words : uint8_t { Saved, Taken, NoCard, Failed, NoMemory, Unavailable };

    // The card's rows: Logic::Effect::kCard.h rows of the screen's width, from kCard.y, 0 black to 255 white.
    inline constexpr int kRows = Logic::Effect::kCard.h;
    inline constexpr int kBytes = 240 * kRows;
    // The card in the system menu's language (its index, system/menu/words.inc), with the shot's `name` (ASCII; may
    // be empty) under the words, and a frame for the thumbnail (`thumb`) or a grey box where it would be.
    void Draw(uint8_t* grey, int language, Words words, const char* name, bool thumb);

    struct Scene {
        uint32_t t = 0;                    // ms since the shot
        const uint16_t* firmware = nullptr;  // the firmware's frame now (240 x 135), or nullptr: black
        const uint16_t* thumb = nullptr;     // the shot at a quarter size (60 x 34), or nullptr
        const uint8_t* card = nullptr;       // Draw's card, or nullptr: a white card
    };
    // Columns [x, x + width) of rows [y, y + rows) of the screen at the scene's moment, RGB565 as the panel takes
    // it: each pixel the firmware's, the flash's white, the card's grey or the thumbnail's (never a mix).
    void Paint(const Scene& scene, int x, int y, int width, int rows, uint16_t* out);

}  // namespace CardenzaHAL::Screenshot::Card
