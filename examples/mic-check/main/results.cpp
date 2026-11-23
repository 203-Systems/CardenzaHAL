#include "results.h"

#include <cstdio>
#include <cstring>

namespace MicCheck {

namespace {

uint32_t Fnv1a(const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t hash = 2166136261u;
    for (std::size_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 16777619u;
    }
    return hash;
}

uint32_t ChecksumOf(const Results& results) { return Fnv1a(&results, offsetof(Results, checksum)); }

}  // namespace

void ClearResults(Results& results, uint32_t run)
{
    std::memset(&results, 0, sizeof(results));
    results.magic = kResultsMagic;
    results.layout = kResultsLayout;
    results.run = run;
    results.sampleRate = kSampleRate;
    results.bits = 16;
    for (auto& take : results.takes) take.levels = EmptyLevels();
    Seal(results);
}

void Seal(Results& results) { results.checksum = ChecksumOf(results); }

bool Intact(const Results& results)
{
    if (results.magic != kResultsMagic || results.layout != kResultsLayout) return false;
    if (results.checksum != ChecksumOf(results)) return false;
    for (const auto& take : results.takes)
        if (take.state > kSkipped) return false;
    return true;
}

int CountState(const Results& results, TakeState state)
{
    int count = 0;
    for (const auto& take : results.takes) count += take.state == state ? 1 : 0;
    return count;
}

int FirstOpenStep(const Results& results)
{
    for (int step = 0; step < kStepCount; ++step)
        if (results.takes[step].state == kNotDone) return step;
    return kStepCount;
}

int FormatLine(char* out, std::size_t size, const char* kind, const Results& results, int step)
{
    const Step& plan = kPlan[step];
    const TakeRecord& take = results.takes[step];
    const int head = std::snprintf(out, size, "MICTEST %s%stake=%d run=%u dist=%s level=%s", kind, *kind != 0 ? " " : "",
                                   step + 1, static_cast<unsigned>(results.run), plan.distance, SoundKey(plan.sound));
    if (head < 0 || static_cast<std::size_t>(head) >= size) return head;
    if (take.state != kMeasured)
        return head + std::snprintf(out + head, size - head, take.state == kSkipped ? " skipped=1" : " open=1");
    const Levels& l = take.levels;
    return head + std::snprintf(out + head, size - head,
                                " rms=%.1f peak=%.1f p95=%.1f p10=%.1f loud1s=%.1f clip=%u fs=%.1f dc=%.1f ovf=%u n=%u"
                                " rate=%u bits=%u ch=1 gain=raw",
                                l.rms, l.peak, l.p95, l.p10, l.loud1s, static_cast<unsigned>(l.clipped), l.fullScale,
                                l.dc, static_cast<unsigned>(take.overflows), static_cast<unsigned>(l.samples),
                                static_cast<unsigned>(results.sampleRate), static_cast<unsigned>(results.bits));
}

int FormatTableHeader(char* out, std::size_t size, const Results& results, int boardMicGainDb)
{
    return std::snprintf(out, size,
                         "MICTEST table run=%u steps=%d measured=%d skipped=%d rate=%u bits=%u ch=1 gain=raw unit=dBFS"
                         " board_mic_gain=%d",
                         static_cast<unsigned>(results.run), kStepCount, CountState(results, kMeasured),
                         CountState(results, kSkipped), static_cast<unsigned>(results.sampleRate),
                         static_cast<unsigned>(results.bits), boardMicGainDb);
}

}  // namespace MicCheck
