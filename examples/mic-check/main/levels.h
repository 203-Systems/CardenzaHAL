#pragma once

// What a take of the raw microphone measures. Pure (no ESP-IDF): the firmware and the host test
// (test/levels_test.cpp) run the same code.
//
// The samples are the HAL's PDM microphone as it delivers them (Mic::InputChannel: 44,100 Hz, mono, signed
// 16-bit), before any gain (no Mic::ApplyGain). Levels are dBFS relative to the 16-bit full scale, 32768:
// a sample of -32768 is 0 dBFS peak, and a full-scale sine has an RMS of -3.0 dBFS. The sums are exact
// integers (each square fits 32 bits, the sums 64); only the final levels are floating point.

#include <cstddef>
#include <cstdint>

namespace MicCheck {

inline constexpr uint32_t kSampleRate = 44100;
inline constexpr uint32_t kTakeSeconds = 5;
inline constexpr uint32_t kTakeSamples = kSampleRate * kTakeSeconds;   // 220,500
inline constexpr uint32_t kWindowSamples = kSampleRate / 20;            // 2,205: the 50 ms short-term level
inline constexpr uint32_t kTakeWindows = kTakeSamples / kWindowSamples; // 100
inline constexpr uint32_t kSecondWindows = 20;                          // 1 s of windows
inline constexpr float kFloorDb = -120.0f;                              // what silence reads (log of zero)

// Plain data (no initializers): it is kept in RTC memory as it is (results.h).
struct Levels
{
    uint32_t samples;   // samples measured (kTakeSamples for a whole take)
    float rms;          // dBFS of the whole take, its DC removed
    float peak;         // dBFS of the largest |sample|
    float p95;          // the 95th percentile of the 50 ms levels (each window's own mean removed)
    float p10;          // the 10th percentile: the quiet between words, the floor under the sound
    float loud1s;       // the loudest second: 20 windows in a row, the most energy
    float dc;           // the mean sample, in LSB
    float fullScale;    // the largest |sample| in % of full scale (32768)
    uint32_t clipped;   // samples at full scale (|x| >= 32767)
};

Levels EmptyLevels();

// Mean square (in LSB^2) to dBFS, at kFloorDb for nothing.
float PowerToDb(double meanSquare);
// A sample's magnitude (0..32768) to dBFS.
float MagnitudeToDb(uint32_t magnitude);

class TakeAnalyzer
{
public:
    void Reset();
    // Takes at most what the take still needs (kTakeSamples in all); returns how many it took.
    std::size_t Add(const int16_t* samples, std::size_t count);
    bool Full() const { return count_ >= kTakeSamples; }
    uint32_t Count() const { return count_; }
    Levels Finish() const;

private:
    int64_t sum_ = 0;
    uint64_t squares_ = 0;
    uint32_t count_ = 0;
    uint32_t maxMagnitude_ = 0;
    uint32_t clipped_ = 0;
    int32_t windowSum_ = 0;
    uint64_t windowSquares_ = 0;
    uint32_t windowCount_ = 0;
    uint32_t windows_ = 0;
    int32_t windowSums_[kTakeWindows] = {};
    uint64_t windowSquaresAll_[kTakeWindows] = {};
};

// The meter's level of a block: its mean square with the block's own mean removed, in dBFS; the largest
// magnitude in *peak.
float BlockDb(const int16_t* samples, std::size_t count, uint32_t* peak);

}  // namespace MicCheck
