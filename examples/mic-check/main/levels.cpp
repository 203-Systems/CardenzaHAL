#include "levels.h"

#include <algorithm>
#include <cmath>

namespace MicCheck {

namespace {

constexpr double kFullScale = 32768.0;

// The nearest-rank percentile of n sorted values (0 < fraction <= 1).
float Percentile(const float* sorted, uint32_t n, double fraction)
{
    if (n == 0) return kFloorDb;
    uint32_t rank = static_cast<uint32_t>(std::ceil(fraction * n));
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    return sorted[rank - 1];
}

// The mean square of n samples with their own mean removed. Exact sums in, one rounding out.
double AcPower(int64_t sum, uint64_t squares, uint32_t n)
{
    if (n == 0) return 0.0;
    const double mean = static_cast<double>(sum) / n;
    const double power = static_cast<double>(squares) / n - mean * mean;
    return power > 0.0 ? power : 0.0;
}

uint32_t Magnitude(int32_t sample) { return static_cast<uint32_t>(sample < 0 ? -sample : sample); }

}  // namespace

Levels EmptyLevels()
{
    Levels levels;
    levels.samples = 0;
    levels.rms = levels.peak = levels.p95 = levels.p10 = levels.loud1s = kFloorDb;
    levels.dc = 0.0f;
    levels.fullScale = 0.0f;
    levels.clipped = 0;
    return levels;
}

float PowerToDb(double meanSquare)
{
    if (!(meanSquare > 0.0)) return kFloorDb;
    const double db = 10.0 * std::log10(meanSquare / (kFullScale * kFullScale));
    return db < kFloorDb ? kFloorDb : static_cast<float>(db);
}

float MagnitudeToDb(uint32_t magnitude)
{
    if (magnitude == 0) return kFloorDb;
    const double db = 20.0 * std::log10(magnitude / kFullScale);
    return db < kFloorDb ? kFloorDb : static_cast<float>(db);
}

void TakeAnalyzer::Reset() { *this = TakeAnalyzer(); }

std::size_t TakeAnalyzer::Add(const int16_t* samples, std::size_t count)
{
    const std::size_t room = kTakeSamples - count_;
    if (count > room) count = room;
    for (std::size_t index = 0; index < count; ++index)
    {
        const int32_t sample = samples[index];
        const uint32_t square = static_cast<uint32_t>(sample * sample);  // at most 2^30
        const uint32_t magnitude = Magnitude(sample);
        sum_ += sample;
        squares_ += square;
        if (magnitude > maxMagnitude_) maxMagnitude_ = magnitude;
        if (magnitude >= 32767) ++clipped_;
        windowSum_ += sample;
        windowSquares_ += square;
        if (++windowCount_ == kWindowSamples)
        {
            windowSums_[windows_] = windowSum_;
            windowSquaresAll_[windows_] = windowSquares_;
            ++windows_;
            windowSum_ = 0;
            windowSquares_ = 0;
            windowCount_ = 0;
        }
    }
    count_ += static_cast<uint32_t>(count);
    return count;
}

Levels TakeAnalyzer::Finish() const
{
    Levels levels = EmptyLevels();
    levels.samples = count_;
    if (count_ == 0) return levels;
    levels.dc = static_cast<float>(static_cast<double>(sum_) / count_);
    levels.rms = PowerToDb(AcPower(sum_, squares_, count_));
    levels.peak = MagnitudeToDb(maxMagnitude_);
    levels.fullScale = static_cast<float>(maxMagnitude_ * 100.0 / kFullScale);
    levels.clipped = clipped_;

    // The short-term levels: whole 50 ms windows (a take shorter than one is its own window).
    double powers[kTakeWindows];
    float sorted[kTakeWindows];
    uint32_t windows = windows_;
    if (windows == 0)
    {
        powers[0] = AcPower(sum_, squares_, count_);
        windows = 1;
    }
    else
    {
        for (uint32_t window = 0; window < windows; ++window)
            powers[window] = AcPower(windowSums_[window], windowSquaresAll_[window], kWindowSamples);
    }
    for (uint32_t window = 0; window < windows; ++window) sorted[window] = PowerToDb(powers[window]);
    std::sort(sorted, sorted + windows);
    levels.p95 = Percentile(sorted, windows, 0.95);
    levels.p10 = Percentile(sorted, windows, 0.10);

    // The loudest second: the most energy in kSecondWindows windows in a row (all of them when fewer).
    const uint32_t span = windows < kSecondWindows ? windows : kSecondWindows;
    double run = 0.0;
    for (uint32_t window = 0; window < span; ++window) run += powers[window];
    double loudest = run;
    for (uint32_t window = span; window < windows; ++window)
    {
        run += powers[window] - powers[window - span];
        if (run > loudest) loudest = run;
    }
    levels.loud1s = PowerToDb(loudest / span);
    return levels;
}

float BlockDb(const int16_t* samples, std::size_t count, uint32_t* peak)
{
    int64_t sum = 0;
    uint64_t squares = 0;
    uint32_t largest = 0;
    for (std::size_t index = 0; index < count; ++index)
    {
        const int32_t sample = samples[index];
        sum += sample;
        squares += static_cast<uint32_t>(sample * sample);
        const uint32_t magnitude = Magnitude(sample);
        if (magnitude > largest) largest = magnitude;
    }
    if (peak != nullptr) *peak = largest;
    return PowerToDb(AcPower(sum, squares, static_cast<uint32_t>(count)));
}

}  // namespace MicCheck
