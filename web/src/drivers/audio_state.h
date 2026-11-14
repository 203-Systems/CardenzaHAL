#pragma once

#include <cstddef>
#include <cstdint>

// The speaker path the page plays: the ES8156 codec's state and the frames its I2S input carries.
namespace cardenza_web::audio {

constexpr uint32_t kRate = 44100;

// The page's audio clock, in frames at kRate since the page started (web_main.cpp: the AudioContext
// once it runs, the performance clock before).
double ClockFrames();

// The codec (i2c_codec.cpp): linear gain of its volume register, 0 while muted or in standby.
float CodecGain();
bool CodecRunning();

// Mono frames the codec turns into sound, at an absolute clock position (i2s.cpp). Gaps are silence.
void Speaker(int64_t position, const float* frames, std::size_t count);
// The page's side: the next contiguous frames (count returned, position in *position).
std::size_t TakeSpeaker(float* out, std::size_t max, double* position);

// The microphone (the page's input, or silence): fills `count` frames from an absolute position.
void Microphone(int64_t position, int16_t* frames, std::size_t count);
void PushMicrophone(double position, const float* frames, std::size_t count);
bool MicrophoneWanted();

// GPIO43 is the speaker's WS and the microphone's clock: the side that configured it last owns it.
enum class Pin43 { Speaker, Microphone, Nobody };
void ClaimPin43(Pin43 owner);
Pin43 Pin43Owner();

}  // namespace cardenza_web::audio
