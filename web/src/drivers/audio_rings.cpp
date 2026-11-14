// The speaker's frames on their way to the page, and the page's microphone on its way to I2S: rings
// addressed by absolute position on the audio clock. Pure: tested on the host (tests/web).

#include <algorithm>
#include <cstring>

#include "audio_state.h"

namespace cardenza_web::audio {

namespace {

constexpr std::size_t kSpeakerFrames = 1 << 16;  // about 1.5 s
constexpr std::size_t kSegments = 64;

struct Segment {
    int64_t position;
    std::size_t start;  // index in the ring
    std::size_t count;
};

float g_speaker[kSpeakerFrames];
std::size_t g_speakerHead = 0;   // first unread frame
std::size_t g_speakerCount = 0;  // unread frames
Segment g_segments[kSegments];
std::size_t g_segmentHead = 0;
std::size_t g_segmentCount = 0;

constexpr std::size_t kMicFrames = 1 << 16;
int16_t g_mic[kMicFrames];
int64_t g_micEnd = 0;  // absolute position after the last frame the page gave

Pin43 g_pin43 = Pin43::Nobody;

}  // namespace

void Speaker(int64_t position, const float* frames, std::size_t count)
{
    while (count > 0)
    {
        if (g_speakerCount == kSpeakerFrames || g_segmentCount == kSegments)
        {
            // The page stopped taking frames: the oldest go.
            if (g_segmentCount == 0) return;
            Segment& oldest = g_segments[g_segmentHead];
            g_speakerHead = (g_speakerHead + oldest.count) % kSpeakerFrames;
            g_speakerCount -= oldest.count;
            g_segmentHead = (g_segmentHead + 1) % kSegments;
            --g_segmentCount;
            continue;
        }
        Segment* last = g_segmentCount > 0 ? &g_segments[(g_segmentHead + g_segmentCount - 1) % kSegments] : nullptr;
        if (last == nullptr || last->position + static_cast<int64_t>(last->count) != position)
        {
            Segment& segment = g_segments[(g_segmentHead + g_segmentCount) % kSegments];
            segment.position = position;
            segment.start = (g_speakerHead + g_speakerCount) % kSpeakerFrames;
            segment.count = 0;
            ++g_segmentCount;
            last = &segment;
        }
        const std::size_t room = std::min(kSpeakerFrames - g_speakerCount, count);
        for (std::size_t index = 0; index < room; ++index) g_speaker[(g_speakerHead + g_speakerCount + index) % kSpeakerFrames] = frames[index];
        g_speakerCount += room;
        last->count += room;
        position += static_cast<int64_t>(room);
        frames += room;
        count -= room;
    }
}

std::size_t TakeSpeaker(float* out, std::size_t max, double* position)
{
    if (g_segmentCount == 0) return 0;
    Segment& segment = g_segments[g_segmentHead];
    const std::size_t count = std::min(max, segment.count);
    for (std::size_t index = 0; index < count; ++index) out[index] = g_speaker[(segment.start + index) % kSpeakerFrames];
    *position = static_cast<double>(segment.position);
    segment.position += static_cast<int64_t>(count);
    segment.start = (segment.start + count) % kSpeakerFrames;
    segment.count -= count;
    g_speakerHead = (g_speakerHead + count) % kSpeakerFrames;
    g_speakerCount -= count;
    if (segment.count == 0)
    {
        g_segmentHead = (g_segmentHead + 1) % kSegments;
        --g_segmentCount;
    }
    return count;
}

void PushMicrophone(double position, const float* frames, std::size_t count)
{
    int64_t at = static_cast<int64_t>(position);
    for (std::size_t index = 0; index < count; ++index, ++at)
    {
        const float clipped = std::clamp(frames[index], -1.0f, 1.0f);
        g_mic[static_cast<uint64_t>(at) % kMicFrames] = static_cast<int16_t>(clipped * 32767.0f);
    }
    if (at > g_micEnd) g_micEnd = at;
}

// The page hands the microphone over a few milliseconds after it heard it: I2S reads it this late.
constexpr int64_t kMicrophoneDelay = 2048;

void Microphone(int64_t position, int16_t* frames, std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index)
    {
        const int64_t at = position + static_cast<int64_t>(index) - kMicrophoneDelay;
        const bool known = at < g_micEnd && at >= g_micEnd - static_cast<int64_t>(kMicFrames);
        frames[index] = known ? g_mic[static_cast<uint64_t>(at) % kMicFrames] : 0;
    }
}

void ClaimPin43(Pin43 owner) { g_pin43 = owner; }
Pin43 Pin43Owner() { return g_pin43; }

}  // namespace cardenza_web::audio
