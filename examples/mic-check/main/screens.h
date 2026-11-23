#pragma once

// The test's screens, drawn into a 240 x 135 RGB565 framebuffer from a View. Pure (no ESP-IDF): the host
// test draws every screen into PNGs (test/shots.cpp).

#include <cstdint>

#include "results.h"

namespace MicCheck {

enum class Screen : uint8_t
{
    Intro,      // what the test is; OK starts it
    Ready,      // a step's instruction; OK records it
    Recording,  // the 5 s take: countdown, live level
    Result,     // the take's numbers; OK next, ESC again
    Table,      // every step's numbers, scrolled with the arrows
};

enum class CardState : uint8_t
{
    None,     // no card in this run: nothing is written
    Ready,    // mounted
    Saving,   // the take is being written
    Saved,    // the step's take is on the card
    Failed,   // a write failed: no more writes this run
};

struct View
{
    Screen screen = Screen::Intro;
    int step = 0;                     // Ready, Recording, Result
    const Results* results = nullptr;
    float levelDb = kFloorDb;         // the live level (50 ms)
    float peakHoldDb = kFloorDb;      // the live peak, held
    float elapsed = 0.0f;             // Recording: seconds of the take so far
    bool waiting = false;             // Ready: OK pressed while the card is busy; the take starts after
    int tableFirst = 0;               // Table: the first row shown
    bool fromBoot = false;            // Table: results kept through a reset
    bool canResume = false;           // Table: steps remain; Intro: there are results to go back to
    CardState card = CardState::None;
    int run = 0;                      // the card's folder run-NN (0: none)
};

inline constexpr int kTableRows = 6;  // rows the table shows at once

void Draw(uint16_t* frame, const View& view);

}  // namespace MicCheck
