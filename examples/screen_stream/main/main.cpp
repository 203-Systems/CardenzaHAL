// The screen stream's test firmware (usb/camera.h, docs/screen-stream.md): the camera on from the first
// enumeration, and four scenes that cost it differently, each for 10 s in turn (keys 1 to 4 hold one; 0 goes back
// to the turns):
//   1 idle: a still page             2 playhead: a 2-pixel bar walking over a grid, a step every 6 frames
//   3 page change: two pages, 1 s each   4 full-screen animation: every pixel, every frame
// A quiet tone plays on the speaker (and to the computer, when it records the device) the whole time, so audio
// underruns show up. Every 5 s the log has a line "stream: ..." with what the stream sent and cost (203cli log, or
// the console over the system channel); tools/web/screen-stream-demo.html shows the picture and the same numbers.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>

#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "cardenzaHAL.h"
#include "usb/camera.h"
#include "usb/usb.h"

namespace {

namespace Display = CardenzaHAL::Display;
namespace Keyboard = CardenzaHAL::Keyboard;
namespace Camera = CardenzaHAL::USB::Camera;

constexpr char kTag[] = "stream";
constexpr int W = Display::WIDTH, H = Display::HEIGHT;
alignas(4) uint16_t g_frame[W * H];

constexpr uint16_t Rgb(int r, int g, int b) { return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }

void Fill(int x, int y, int w, int h, uint16_t colour)
{
    for (int row = std::max(y, 0); row < y + h && row < H; ++row)
        for (int column = std::max(x, 0); column < x + w && column < W; ++column) g_frame[row * W + column] = colour;
}

// A 4x4 Bayer dither, as the groovebox fills areas.
void Dither(int x, int y, int w, int h, uint16_t colour, int level)
{
    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for (int row = y; row < y + h && row < H; ++row)
        for (int column = x; column < x + w && column < W; ++column) g_frame[row * W + column] = bayer[row & 3][column & 3] < level ? colour : 0;
}

// Marks that look like text (this example has no font).
void Marks(int x, int y, int count, uint16_t colour, unsigned seed)
{
    std::minstd_rand random(seed);
    for (int index = 0; index < count; ++index)
        for (int stroke = 0; stroke < 4; ++stroke)
            Fill(x + index * 6 + static_cast<int>(random() % 4), y + static_cast<int>(random() % 6), 1 + static_cast<int>(random() % 3),
                 1 + static_cast<int>(random() % 3), colour);
}

// A sequencer-like page: header, a 4x4 grid of cells with labels and dithered fills, footer; the scene number as
// that many blocks at the top right.
void Page(unsigned seed, int scene)
{
    std::minstd_rand random(seed);
    Fill(0, 0, W, H, 0);
    Marks(4, 2, 20, Rgb(232, 232, 224), seed);
    for (int index = 0; index < scene; ++index) Fill(W - 8 - index * 8, 2, 6, 6, Rgb(255, 208, 48));
    Fill(0, 11, W, 1, Rgb(58, 58, 54));
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
        {
            const int x = 10 + column * 55, y = 14 + row * 27;
            if (random() % 3 == 0) Dither(x, y, 52, 24, Rgb(72, 255, 120), 4 + static_cast<int>(random() % 8));
            Marks(x + 2, y + 2, 2, Rgb(132, 132, 126), seed + row * 4 + column);
            Marks(x + 2, y + 12, 3, Rgb(232, 232, 224), seed * 7 + row * 4 + column);
            Fill(x, y + 22, 50, 2, Rgb(58, 58, 54));
        }
    Marks(10, 127, 18, Rgb(160, 112, 255), seed + 99);
}

void Draw(int scene, uint32_t frame)
{
    switch (scene)
    {
        case 1: Page(1, scene); break;
        case 2:
            Page(1, scene);
            Fill(10 + static_cast<int>((frame / 6) % 16) * 13, 14, 2, 108, Rgb(72, 255, 120));
            break;
        case 3: Page((frame / 50) % 2 ? 2 : 1, scene); break;
        default:
        {
            std::minstd_rand random(frame);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                {
                    const int r = (x + static_cast<int>(frame) * 3) & 31, g = (y * 2 + static_cast<int>(frame)) & 63;
                    const int b = (static_cast<int>(random() & 3) + ((x ^ y) & 15)) & 31;
                    g_frame[y * W + x] = static_cast<uint16_t>(r << 11 | g << 5 | b);
                }
            for (int index = 0; index < scene; ++index) Fill(W - 8 - index * 8, 2, 6, 6, Rgb(255, 208, 48));
        }
    }
}

// A quiet 440 Hz tone, 256 frames a block, to the speaker and (when it records) the computer.
void Tone(void*)
{
    i2s_chan_handle_t tx = CardenzaHAL::Audio::GetTxChannel();
    if (tx == nullptr || i2s_channel_enable(tx) != ESP_OK)
    {
        ESP_LOGW(kTag, "no speaker: the tone is off");
        vTaskDelete(nullptr);
    }
    constexpr int kBlock = 256;
    int16_t mono[kBlock], stereo[2 * kBlock];
    float phase = 0;
    const float step = 2.0f * 3.14159265f * 440.0f / CardenzaHAL::USB::kSampleRate;
    for (;;)
    {
        for (int index = 0; index < kBlock; ++index)
        {
            mono[index] = static_cast<int16_t>(1500.0f * std::sin(phase));
            stereo[2 * index] = stereo[2 * index + 1] = mono[index];
            phase += step;
            if (phase > 6.2831853f) phase -= 6.2831853f;
        }
        CardenzaHAL::USB::AudioWrite(stereo, kBlock);
        size_t written = 0;
        i2s_channel_write(tx, mono, sizeof(mono), &written, portMAX_DELAY);
    }
}

}  // namespace

extern "C" void app_main()
{
    CardenzaHAL::Init();
    // The camera from the first enumeration: a firmware's own USB::Init within CardenzaHAL::Init's second takes the
    // device over with its classes (usb/usb.h). The camera's endpoint comes from what gives way (the playback's
    // feedback endpoint: adaptive playback).
    CardenzaHAL::USB::Config usb;
    usb.product = "screen_stream";
    usb.camera = true;
    if (!CardenzaHAL::USB::Init(usb) && !Camera::Enabled()) Camera::Enable(true);
    ESP_LOGI(kTag, "camera %s", Camera::Enabled() ? "on" : "off (CONFIG_CARDENZA_HAL_USB_CAMERA? PSRAM?)");
    xTaskCreatePinnedToCore(Tone, "tone", 4096, nullptr, 10, nullptr, 1);
    Keyboard::enableLED(true);

    int held = 0;  // a scene a key holds (0: the scenes take turns)
    uint32_t frame = 0;
    Display::FramePacer pacer;
    Camera::Stats before = Camera::GetStats();
    CardenzaHAL::Audio::TxCounters audioBefore = CardenzaHAL::Audio::GetTxCounters();
    Display::ResetTotals();
    for (;;)
    {
        Keyboard::updateKeyList();
        for (const auto& key : Keyboard::keyList())
            if (key.y == 0 && key.x >= 1 && key.x <= 4) held = key.x;
            else if (key.y == 0 && key.x == 10) held = 0;  // the key 0
        const int scene = held != 0 ? held : 1 + static_cast<int>((pacer.Tick() / 1000) % 4);
        if (pacer.DrawTick())
        {
            Draw(scene, frame++);
            Display::PresentChanged(g_frame);
        }
        if (pacer.Tick() % 500 == 499)
        {
            const Camera::Stats now = Camera::GetStats();
            const auto audio = CardenzaHAL::Audio::GetTxCounters();
            const Display::PresentTotals totals = Display::Totals();
            const unsigned frames = now.frames - before.frames;
            ESP_LOGI(kTag,
                     "scene %d: %s, %u frames in 5 s (%.1f fps), %.1f KB/s, %u keyframes, %u coalesced | capture %u us "
                     "(max %u), write %u us (max %u) | present %u us | audio blocks %u, empty %u | internal %u KB, PSRAM %u KB",
                     scene, now.streaming ? (now.stalled ? "stalled" : "streaming") : (now.open ? "no reader" : "not open"), frames,
                     frames / 5.0, (now.bytes - before.bytes) / 5.0 / 1024.0, unsigned(now.keyframes - before.keyframes),
                     unsigned(now.coalesced - before.coalesced), unsigned(now.captureUs), unsigned(now.maxCaptureUs),
                     unsigned(now.encodeUs), unsigned(now.maxEncodeUs),
                     totals.frames ? unsigned(totals.presentUs / totals.frames) : 0u, unsigned(audio.sent - audioBefore.sent),
                     unsigned(audio.empty - audioBefore.empty), unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            before = now;
            audioBefore = audio;
            Display::ResetTotals();
        }
        pacer.Wait();
    }
}
