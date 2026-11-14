// The speaker and microphone rings (web/src/drivers/audio_rings.cpp): frames keep their positions,
// a gap starts a new run, the page takes runs in order; the microphone is read a little late.
#include <cassert>
#include <cstdio>
#include <vector>

#include "audio_state.h"

namespace a = cardenza_web::audio;

int main()
{
    std::vector<float> frames(300);
    for (size_t index = 0; index < frames.size(); ++index) frames[index] = static_cast<float>(index);
    a::Speaker(1000, frames.data(), 100);
    a::Speaker(1100, frames.data() + 100, 100);  // contiguous: one run
    a::Speaker(5000, frames.data() + 200, 50);   // a gap: a second run
    float out[512];
    double position = 0;
    size_t count = a::TakeSpeaker(out, 150, &position);
    assert(count == 150 && position == 1000 && out[0] == 0 && out[149] == 149);
    count = a::TakeSpeaker(out, 512, &position);
    assert(count == 50 && position == 1150 && out[0] == 150);
    count = a::TakeSpeaker(out, 512, &position);
    assert(count == 50 && position == 5000 && out[0] == 200);
    assert(a::TakeSpeaker(out, 512, &position) == 0);

    // An untaken ring keeps the newest frames.
    std::vector<float> lots(1 << 16, 0.5f);
    for (int round = 0; round < 3; ++round) a::Speaker(100000 + round * (1 << 16), lots.data(), lots.size());
    count = a::TakeSpeaker(out, 512, &position);
    assert(count > 0 && position >= 100000 + 2 * (1 << 16) - 512);

    // The microphone: what the page pushed comes back 2048 frames later; unknown frames are silence.
    std::vector<float> heard(4096, 0.25f);
    a::PushMicrophone(10000, heard.data(), heard.size());
    int16_t mic[16];
    a::Microphone(10000 + 2048, mic, 16);
    assert(mic[0] == static_cast<int16_t>(0.25f * 32767.0f));
    a::Microphone(10000 + 4096 + 2048, mic, 16);
    assert(mic[0] == 0);

    a::ClaimPin43(a::Pin43::Microphone);
    assert(a::Pin43Owner() == a::Pin43::Microphone);
    std::printf("audio ring tests passed\n");
    return 0;
}
