// mic-check's level math against signals whose levels are known, the results' checksum and the log lines.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "levels.h"
#include "results.h"

using namespace MicCheck;

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

void Near(double value, double expected, double tolerance, const std::string& what)
{
    Check(std::fabs(value - expected) <= tolerance,
          what + ": " + std::to_string(value) + ", expected " + std::to_string(expected));
}

int16_t Clamp(double value)
{
    const double rounded = std::floor(value + 0.5);
    return static_cast<int16_t>(rounded > 32767 ? 32767 : rounded < -32768 ? -32768 : rounded);
}

std::vector<int16_t> Sine(double amplitude, double hertz, double offset = 0.0, uint32_t count = kTakeSamples)
{
    std::vector<int16_t> samples(count);
    for (uint32_t index = 0; index < count; ++index)
        samples[index] = Clamp(offset + amplitude * std::sin(2.0 * M_PI * hertz * index / kSampleRate));
    return samples;
}

// Fed in uneven pieces, so windows cross the calls.
Levels Measure(const std::vector<int16_t>& samples, std::size_t piece = 517)
{
    TakeAnalyzer analyzer;
    analyzer.Reset();
    for (std::size_t at = 0; at < samples.size(); at += piece)
        analyzer.Add(samples.data() + at, std::min(piece, samples.size() - at));
    return analyzer.Finish();
}

// The reference: the same quantities straight from the definition, in double precision.
double ReferenceRmsDb(const std::vector<int16_t>& samples)
{
    double mean = 0.0;
    for (int16_t sample : samples) mean += sample;
    mean /= samples.size();
    double power = 0.0;
    for (int16_t sample : samples) power += (sample - mean) * (sample - mean);
    power /= samples.size();
    return 10.0 * std::log10(power / (32768.0 * 32768.0));
}

double SineRmsDb(double amplitude) { return 20.0 * std::log10(amplitude / std::sqrt(2.0) / 32768.0); }

void TestSilence()
{
    const Levels l = Measure(std::vector<int16_t>(kTakeSamples, 0));
    Check(l.samples == kTakeSamples, "silence: samples");
    Check(l.rms == kFloorDb && l.peak == kFloorDb && l.p95 == kFloorDb && l.p10 == kFloorDb && l.loud1s == kFloorDb,
          "silence reads the floor");
    Check(l.dc == 0.0f && l.clipped == 0 && l.fullScale == 0.0f, "silence: no DC, nothing clipped");
}

void TestSines()
{
    const Levels half = Measure(Sine(16384, 1000));
    Near(half.rms, SineRmsDb(16384), 0.01, "half-scale sine rms");
    Near(half.rms, -9.03, 0.01, "half-scale sine rms in dBFS");
    Near(half.peak, -6.02, 0.01, "half-scale sine peak");
    Near(half.p95, half.rms, 0.05, "steady sine: p95 is its rms");
    Near(half.p10, half.rms, 0.05, "steady sine: p10 is its rms");
    Near(half.loud1s, half.rms, 0.02, "steady sine: the loudest second is its rms");
    Near(half.fullScale, 50.0, 0.01, "half-scale sine: 50% of full scale");
    Near(half.dc, 0.0, 0.05, "sine: no DC");
    Check(half.clipped == 0, "half-scale sine: nothing clipped");

    const Levels full = Measure(Sine(32767, 997));
    Near(full.rms, -3.01, 0.01, "full-scale sine rms is -3 dBFS");
    Near(full.peak, 0.0, 0.001, "full-scale sine peak");

    const auto quiet = Sine(3.0, 440);
    Near(Measure(quiet).rms, ReferenceRmsDb(quiet), 0.001, "a 3 LSB sine against the reference");
}

void TestClipping()
{
    const auto samples = Sine(2.0 * 32767, 220);
    uint32_t expected = 0;
    for (int16_t sample : samples) expected += (sample >= 32767 || sample <= -32767) ? 1 : 0;
    const Levels l = Measure(samples);
    Check(l.clipped == expected && expected > 1000, "clipped samples: " + std::to_string(l.clipped) + " of " +
                                                         std::to_string(expected));
    Near(l.peak, 0.0, 1e-6, "a clipped take peaks at 0 dBFS");
    Near(l.fullScale, 100.0, 1e-6, "a clipped take reaches 100% of full scale");

    const Levels floor = Measure(std::vector<int16_t>(kTakeSamples, -32768));
    Check(floor.clipped == kTakeSamples && floor.rms == kFloorDb, "a constant -32768: all clipped, no AC level");
    Near(floor.dc, -32768.0, 1e-3, "a constant -32768: its DC");
}

void TestDc()
{
    const Levels l = Measure(Sine(1000, 1000, 1000.0));
    Near(l.dc, 1000.0, 0.05, "DC offset");
    Near(l.rms, SineRmsDb(1000), 0.01, "rms with the DC removed");
}

void TestNoise()
{
    std::vector<int16_t> samples(kTakeSamples);
    uint32_t state = 12345;
    for (auto& sample : samples)
    {
        state = state * 1664525u + 1013904223u;
        sample = static_cast<int16_t>(static_cast<int32_t>(state >> 16 & 0x7FF) - 1024);
    }
    const Levels l = Measure(samples);
    Near(l.rms, ReferenceRmsDb(samples), 0.001, "noise rms against the reference");
    Near(l.rms, 20.0 * std::log10(2048.0 / std::sqrt(12.0) / 32768.0), 0.05, "uniform noise rms");
    Check(l.p10 < l.rms && l.p95 > l.rms && l.p95 - l.p10 < 1.0, "noise: the short-term levels spread a little");
}

void TestSpread()
{
    // 2.5 s at -53 dB, then 2.5 s at -13: the 10th percentile is the quiet half, the 95th the loud one.
    std::vector<int16_t> samples = Sine(100, 1000, 0.0, kTakeSamples / 2);
    const auto loud = Sine(10000, 1000, 0.0, kTakeSamples / 2);
    samples.insert(samples.end(), loud.begin(), loud.end());
    const Levels l = Measure(samples);
    Near(l.p10, SineRmsDb(100), 0.1, "two halves: p10");
    Near(l.p95, SineRmsDb(10000), 0.05, "two halves: p95");
    Near(l.loud1s, SineRmsDb(10000), 0.05, "two halves: the loudest second");
    Near(l.rms, ReferenceRmsDb(samples), 0.001, "two halves: rms against the reference");

    // One second at -7.3 dB (windows 40..59) in -44 dB: the loudest second finds it.
    std::vector<int16_t> burst = Sine(200, 500);
    const auto strong = Sine(20000, 500);
    std::copy(strong.begin() + 40 * kWindowSamples, strong.begin() + 60 * kWindowSamples, burst.begin() + 40 * kWindowSamples);
    const Levels b = Measure(burst);
    Near(b.loud1s, SineRmsDb(20000), 0.05, "a one-second burst: the loudest second");
    Near(b.p95, SineRmsDb(20000), 0.05, "a one-second burst: p95 (a fifth of the windows)");
    Near(b.p10, SineRmsDb(200), 0.1, "a one-second burst: p10");
}

void TestPieces()
{
    const auto samples = Sine(5000, 333);
    const Levels whole = Measure(samples, samples.size());
    const Levels pieces = Measure(samples, 7);
    Check(std::memcmp(&whole, &pieces, sizeof(Levels)) == 0, "the same levels whatever the pieces");

    TakeAnalyzer analyzer;
    analyzer.Reset();
    std::vector<int16_t> more(kTakeSamples + 5000, 1);
    Check(analyzer.Add(more.data(), more.size()) == kTakeSamples && analyzer.Full(), "a take stops at 5 s");
    Check(analyzer.Add(more.data(), 10) == 0, "a full take takes nothing more");
}

void TestBlock()
{
    const auto samples = Sine(8192, 1000, 0.0, 512);
    uint32_t peak = 0;
    const float db = BlockDb(samples.data(), samples.size(), &peak);
    Near(db, ReferenceRmsDb(samples), 0.001, "block level");
    Check(peak >= 8190 && peak <= 8192, "block peak");
    Near(MagnitudeToDb(32768), 0.0, 1e-9, "32768 is 0 dBFS");
    Near(MagnitudeToDb(16384), -6.0206, 1e-4, "half scale is -6.02 dBFS");
    Check(PowerToDb(0.0) == kFloorDb && MagnitudeToDb(0) == kFloorDb, "nothing reads the floor");
}

void TestResults()
{
    Results results;
    ClearResults(results, 3);
    Check(Intact(results), "cleared results are intact");
    Check(FirstOpenStep(results) == 0 && CountState(results, kNotDone) == kStepCount, "cleared: every step open");

    results.takes[5].state = kMeasured;
    Check(!Intact(results), "a change without a seal is caught");
    Seal(results);
    Check(Intact(results), "sealed again");
    results.takes[3].levels.rms += 1.0f;
    Check(!Intact(results), "a changed value is caught");
    results.takes[3].levels.rms -= 1.0f;
    Check(Intact(results), "the value back: intact");
    Results garbage;
    std::memset(&garbage, 0xA5, sizeof(garbage));
    Check(!Intact(garbage), "RTC garbage is not results");

    Levels l = EmptyLevels();
    l.samples = kTakeSamples;
    l.rms = -31.24f;
    l.peak = -9.81f;
    l.p95 = -24.0f;
    l.p10 = -50.33f;
    l.loud1s = -27.46f;
    l.dc = -0.31f;
    l.fullScale = 32.38f;
    l.clipped = 0;
    results.takes[5].levels = l;
    results.takes[5].overflows = 0;
    results.takes[4].state = kSkipped;
    Seal(results);
    char line[256];
    FormatLine(line, sizeof(line), "", results, 5);
    Check(std::string(line) ==
              "MICTEST take=6 run=3 dist=30cm level=normal rms=-31.2 peak=-9.8 p95=-24.0 p10=-50.3 loud1s=-27.5"
              " clip=0 fs=32.4 dc=-0.3 ovf=0 n=220500 rate=44100 bits=16 ch=1 gain=raw",
          std::string("measured line: ") + line);
    FormatLine(line, sizeof(line), "row", results, 4);
    Check(std::string(line) == "MICTEST row take=5 run=3 dist=30cm level=soft skipped=1", std::string("skipped line: ") + line);
    FormatLine(line, sizeof(line), "row", results, 0);
    Check(std::string(line) == "MICTEST row take=1 run=3 dist=- level=quiet open=1", std::string("open line: ") + line);
    FormatTableHeader(line, sizeof(line), results, 6);
    Check(std::string(line) ==
              "MICTEST table run=3 steps=16 measured=1 skipped=1 rate=44100 bits=16 ch=1 gain=raw unit=dBFS"
              " board_mic_gain=6",
          std::string("table header: ") + line);
    Check(FirstOpenStep(results) == 0, "the first open step");
}

}  // namespace

int main()
{
    TestSilence();
    TestSines();
    TestClipping();
    TestDc();
    TestNoise();
    TestSpread();
    TestPieces();
    TestBlock();
    TestResults();
    if (g_failures == 0) std::printf("levels: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
