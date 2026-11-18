// The board through the HAL: a firmware to check a Cardenza, or its simulator, key by key.
//
// - The screen shows the 4 x 14 keys. A held key fills its cell, lights its LED and plays a tone (at
//   most four at once: the HAL reads four keys at a time). Keys not held glow faintly.
// - HOME (the side key) switches the key LEDs between full and dim global brightness.
// - A bar at the bottom shows the speaker's level; the square at the top right shows HOME.
//
// The UI runs in the main task at 50 fps and sends only what changed (Display::PresentChanged). The
// sound comes from an audio task on core 1 that writes 256-frame blocks to the HAL's I2S channel;
// it learns which keys are held through a one-item queue (xQueueOverwrite).

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "cardenzaHAL.h"
#include "keyboard/led_light.h"

namespace {

namespace Audio = CardenzaHAL::Audio;
namespace Display = CardenzaHAL::Display;
namespace Keyboard = CardenzaHAL::Keyboard;
namespace System = CardenzaHAL::System;

constexpr char kTag[] = "hal_check";
constexpr int kColumns = 14;
constexpr int kRows = 4;
constexpr int kCellWidth = 16;
constexpr int kCellHeight = 22;
constexpr int kGridX = (Display::WIDTH - kColumns * kCellWidth) / 2;
constexpr int kGridY = 30;
constexpr int kSampleRate = 44100;
constexpr int kBlockFrames = 256;
constexpr int kVoices = 4;

constexpr uint16_t Rgb(int r, int g, int b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr uint16_t kBlack = Rgb(0, 0, 0);
constexpr uint16_t kWhite = Rgb(255, 255, 255);
constexpr uint16_t kFrame = Rgb(58, 58, 54);
constexpr std::array<uint16_t, kRows> kRowColours = {Rgb(255, 90, 54), Rgb(54, 214, 230), Rgb(255, 208, 48), Rgb(160, 112, 255)};

struct Held {
    std::array<uint8_t, kVoices> keys;  // key numbers 1..56, 0 = none
};

QueueHandle_t g_held = nullptr;
std::atomic<int> g_level{0};  // the last block's peak, 0..1000

alignas(4) uint16_t g_frame[Display::WIDTH * Display::HEIGHT];

void Fill(int x, int y, int width, int height, uint16_t colour)
{
    for (int row = y; row < y + height; ++row)
        for (int column = x; column < x + width; ++column)
            if (row >= 0 && row < Display::HEIGHT && column >= 0 && column < Display::WIDTH) g_frame[row * Display::WIDTH + column] = colour;
}

void Outline(int x, int y, int width, int height, uint16_t colour)
{
    Fill(x, y, width, 1, colour);
    Fill(x, y + height - 1, width, 1, colour);
    Fill(x, y, 1, height, colour);
    Fill(x + width - 1, y, 1, height, colour);
}

// Equal temperament from C3, a semitone a key.
float Frequency(uint8_t key) { return 130.81f * std::pow(2.0f, static_cast<float>(key - 1) / 12.0f); }

void AudioTask(void*)
{
    i2s_chan_handle_t tx = Audio::GetTxChannel();
    if (tx == nullptr || i2s_channel_enable(tx) != ESP_OK)
    {
        ESP_LOGE(kTag, "no audio channel");
        vTaskDelete(nullptr);
    }
    Held held{};
    std::array<float, kVoices> phase{};
    std::array<float, kVoices> gain{};
    int16_t block[kBlockFrames];
    for (;;)
    {
        xQueuePeek(g_held, &held, 0);
        int peak = 0;
        for (int frame = 0; frame < kBlockFrames; ++frame)
        {
            float sample = 0.0f;
            for (int voice = 0; voice < kVoices; ++voice)
            {
                // A short ramp in and out: no click when a key starts or stops.
                const float target = held.keys[voice] != 0 ? 0.18f : 0.0f;
                gain[voice] += (target - gain[voice]) * 0.002f;
                if (gain[voice] < 1e-4f && held.keys[voice] == 0) continue;
                if (held.keys[voice] != 0)
                {
                    phase[voice] += Frequency(held.keys[voice]) / kSampleRate;
                    if (phase[voice] >= 1.0f) phase[voice] -= 1.0f;
                }
                sample += gain[voice] * std::sin(6.2831853f * phase[voice]);
            }
            const int value = static_cast<int>(sample * 32767.0f);
            block[frame] = static_cast<int16_t>(value > 32767 ? 32767 : (value < -32768 ? -32768 : value));
            const int magnitude = value < 0 ? -value : value;
            if (magnitude > peak) peak = magnitude;
        }
        g_level.store(peak * 1000 / 32768, std::memory_order_relaxed);
        size_t written = 0;
        // Blocks until the DMA has room: this paces the task at the sample rate.
        i2s_channel_write(tx, block, sizeof(block), &written, portMAX_DELAY);
    }
}

void Draw(const std::vector<Keyboard::Point2D_t>& keys, bool home, bool dim)
{
    Fill(0, 0, Display::WIDTH, Display::HEIGHT, kBlack);
    // A strip in the four row colours, and the HOME square.
    for (int row = 0; row < kRows; ++row) Fill(8 + row * 30, 8, 26, 10, kRowColours[row]);
    Fill(Display::WIDTH - 26, 6, 18, 14, home ? kWhite : kFrame);
    if (dim) Outline(Display::WIDTH - 50, 6, 18, 14, kFrame);
    for (int y = 0; y < kRows; ++y)
        for (int x = 0; x < kColumns; ++x)
        {
            const int left = kGridX + x * kCellWidth;
            const int top = kGridY + y * kCellHeight;
            bool held = false;
            for (const auto& key : keys) held |= key.x == x && key.y == y;
            if (held) Fill(left + 1, top + 1, kCellWidth - 2, kCellHeight - 2, kWhite);
            else Outline(left + 1, top + 1, kCellWidth - 2, kCellHeight - 2, kRowColours[y]);
        }
    const int level = g_level.load(std::memory_order_relaxed);
    Fill(8, Display::HEIGHT - 10, (Display::WIDTH - 16) * level / 1000, 4, Rgb(72, 255, 120));
}

}  // namespace

extern "C" void app_main()
{
    if (!CardenzaHAL::Init())
    {
        const auto& report = CardenzaHAL::LastInitReport();
        ESP_LOGE(kTag, "HAL init failed: system=%d keyboard=%d audio=%d display=%d", report.system, report.keyboard, report.audio,
                 report.display);
        return;
    }
    Audio::SetVolume(90);
    Display::SetBrightness(200);
    Keyboard::enableLED(true);
    Keyboard::setLEDGlobalBrightness(15);
    g_held = xQueueCreate(1, sizeof(Held));
    const Held none{};
    xQueueOverwrite(g_held, &none);
    xTaskCreatePinnedToCore(AudioTask, "audio", 4096, nullptr, 20, nullptr, 1);
    ESP_LOGI(kTag, "ready: hold keys; HOME dims the LEDs");

    bool dim = false;
    bool homeBefore = false;
    std::size_t heldBefore = 0;
    TickType_t wake = xTaskGetTickCount();
    for (;;)
    {
        Keyboard::updateKeyList();
        const auto& keys = Keyboard::keyList();
        const bool home = System::GetHomeButton();
        if (home && !homeBefore)
        {
            dim = !dim;
            Keyboard::setLEDGlobalBrightness(dim ? 4 : 15);
            ESP_LOGI(kTag, "HOME: LEDs %s", dim ? "dim" : "full");
        }
        homeBefore = home;

        Held held{};
        Keyboard::LedFrame leds{};
        leds.fill(Keyboard::Light::PerceivedToLevel(4));
        for (std::size_t index = 0; index < keys.size(); ++index)
        {
            const uint8_t number = Keyboard::getKeyNum(keys[index]);
            if (index < held.keys.size()) held.keys[index] = number;
            leds[keys[index].y * Keyboard::LED_COLUMNS + keys[index].x] = Keyboard::Light::PerceivedToLevel(15);
        }
        if (keys.size() != heldBefore) ESP_LOGI(kTag, "%u keys held", static_cast<unsigned>(keys.size()));
        heldBefore = keys.size();
        xQueueOverwrite(g_held, &held);
        Keyboard::setLEDFrame(leds);

        Draw(keys, home, dim);
        Display::PresentChanged(g_frame);
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(20));
    }
}
