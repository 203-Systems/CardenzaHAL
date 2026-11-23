#pragma once

// The test's results: one record a step, kept in RTC memory so a reset does not lose them (opening the serial
// port can reset the board), and printed as log lines a script can read. Pure (no ESP-IDF).

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "levels.h"
#include "plan.h"

namespace MicCheck {

enum TakeState : uint32_t
{
    kNotDone = 0,
    kMeasured = 1,
    kSkipped = 2,
};

// Plain 32-bit words, no padding: the checksum covers the bytes as they are.
struct TakeRecord
{
    uint32_t state;      // TakeState
    uint32_t overflows;  // DMA buffers the microphone's driver dropped during the take (0: none lost)
    Levels levels;
};

struct Results
{
    uint32_t magic;
    uint32_t layout;
    uint32_t run;        // the test's run number (the card's folder run-NN)
    uint32_t sampleRate;
    uint32_t bits;
    TakeRecord takes[kStepCount];
    uint32_t checksum;   // FNV-1a of everything before it
};

// Kept with RTC_NOINIT_ATTR: no constructor may run at the start, or it would clear the results.
static_assert(std::is_trivially_default_constructible_v<Results>);
static_assert(std::is_trivially_copyable_v<Results>);
static_assert(sizeof(Levels) == 9 * 4 && sizeof(TakeRecord) == 11 * 4, "no padding in the records");
static_assert(sizeof(Results) == (6 + kStepCount * 11) * 4, "no padding in the results");

inline constexpr uint32_t kResultsMagic = 0x4D49434Bu;  // "MICK"
inline constexpr uint32_t kResultsLayout = 1u << 16 | sizeof(Results);

void ClearResults(Results& results, uint32_t run);
void Seal(Results& results);
// The magic, the layout and the checksum hold.
bool Intact(const Results& results);
int CountState(const Results& results, TakeState state);
// The first step not measured or skipped; kStepCount when every step is.
int FirstOpenStep(const Results& results);

// One log line, without a newline:
//   MICTEST take=6 run=2 dist=30cm level=normal rms=-31.2 peak=-9.8 p95=-24.0 p10=-50.3 loud1s=-27.5
//     clip=0 fs=32.4 dc=-0.3 ovf=0 n=220500 rate=44100 bits=16 ch=1 gain=raw
// kind is "" for a take just measured, "row" in the table (MICTEST row take=6 ...); take is the step's
// number from 1. A skipped step says skipped=1, one not done yet open=1, after level=. Returns the length
// (snprintf's).
int FormatLine(char* out, std::size_t size, const char* kind, const Results& results, int step);
// The table's first line: MICTEST table run=2 steps=16 measured=15 skipped=1 rate=44100 bits=16 ch=1 gain=raw
//   unit=dBFS board_mic_gain=<dB> (the board's own setting, which this test never applies).
int FormatTableHeader(char* out, std::size_t size, const Results& results, int boardMicGainDb);

}  // namespace MicCheck
