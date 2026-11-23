// Draws every mic-check screen into PNGs (240 x 135, the panel's pixels), and fails when a screen draws a
// character the font lacks.
//   mic_check_shots <folder>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "canvas.h"
#include "font.h"
#include "screens.h"

using namespace MicCheck;

namespace {

uint32_t Crc(const uint8_t* data, std::size_t size, uint32_t crc = 0xFFFFFFFFu)
{
    for (std::size_t index = 0; index < size; ++index)
    {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

void Be32(std::vector<uint8_t>& out, uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(value >> shift));
}

void Chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data)
{
    Be32(png, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    png.insert(png.end(), body.begin(), body.end());
    Be32(png, Crc(body.data(), body.size()) ^ 0xFFFFFFFFu);
}

// An RGB PNG of the frame, its pixels widened from RGB565; the image data in stored (uncompressed) deflate blocks.
bool WritePng(const std::string& path, const uint16_t* frame)
{
    std::vector<uint8_t> raw;
    for (int y = 0; y < kHeight; ++y)
    {
        raw.push_back(0);
        for (int x = 0; x < kWidth; ++x)
        {
            const uint16_t pixel = frame[y * kWidth + x];
            const int r = pixel >> 11, g = pixel >> 5 & 0x3F, b = pixel & 0x1F;
            raw.push_back(static_cast<uint8_t>(r << 3 | r >> 2));
            raw.push_back(static_cast<uint8_t>(g << 2 | g >> 4));
            raw.push_back(static_cast<uint8_t>(b << 3 | b >> 2));
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    for (std::size_t at = 0; at < raw.size(); at += 65535)
    {
        const std::size_t length = std::min<std::size_t>(65535, raw.size() - at);
        z.push_back(at + length == raw.size() ? 1 : 0);
        z.push_back(static_cast<uint8_t>(length));
        z.push_back(static_cast<uint8_t>(length >> 8));
        z.push_back(static_cast<uint8_t>(~length));
        z.push_back(static_cast<uint8_t>(~length >> 8));
        z.insert(z.end(), raw.begin() + at, raw.begin() + at + length);
    }
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw)
    {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    Be32(z, b << 16 | a);
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> header;
    Be32(header, kWidth);
    Be32(header, kHeight);
    header.insert(header.end(), {8, 2, 0, 0, 0});
    Chunk(png, "IHDR", header);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", {});
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    const bool written = std::fwrite(png.data(), 1, png.size(), file) == png.size();
    return std::fclose(file) == 0 && written;
}

Levels Example(float rms, float peak, float p95, float p10, float loud, uint32_t clipped)
{
    Levels l = EmptyLevels();
    l.samples = kTakeSamples;
    l.rms = rms;
    l.peak = peak;
    l.p95 = p95;
    l.p10 = p10;
    l.loud1s = loud;
    l.dc = -3.4f;
    l.fullScale = 100.0f * std::pow(10.0f, peak / 20.0f);
    l.clipped = clipped;
    return l;
}

// A plausible run: speech loses about 6 dB a doubling of the distance, claps and music are louder.
Results ExampleResults(int measuredSteps)
{
    Results results;
    ClearResults(results, 3);
    const float distanceDb[] = {0.0f, -10.0f, -20.0f, -26.0f};
    for (int step = 0; step < measuredSteps && step < kStepCount; ++step)
    {
        TakeRecord& take = results.takes[step];
        take.state = kMeasured;
        if (step == 0)
        {
            take.levels = Example(-78.4f, -61.9f, -76.8f, -79.0f, -77.9f, 0);
            continue;
        }
        if (step >= 13 && step < 15)
        {
            const float d = step == 13 ? 0.0f : -10.0f;
            take.levels = Example(-38.0f + d, -4.0f + d, -27.0f + d, -36.0f + d, -35.0f + d, step == 13 ? 12 : 0);
            continue;
        }
        if (step == 15)
        {
            take.levels = Example(-42.5f, -21.0f, -37.8f, -47.0f, -41.2f, 0);
            continue;
        }
        const int distance = (step - 1) / 3;
        const int loudness = (step - 1) % 3;
        const float base = -46.0f + distanceDb[distance] + loudness * 9.0f;
        take.levels = Example(base, base + 19.0f, base + 7.0f, base - 22.0f, base + 3.0f, 0);
    }
    results.takes[11].state = kSkipped;
    Seal(results);
    return results;
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string folder = argc > 1 ? argv[1] : ".";
    std::string command = "mkdir -p '" + folder + "'";
    if (std::system(command.c_str()) != 0) return 1;
    static uint16_t frame[kWidth * kHeight];
    const Results partial = ExampleResults(6);
    const Results complete = ExampleResults(kStepCount);
    int failures = 0;
    auto shot = [&](const char* name, const View& view) {
        Draw(frame, view);
        if (!WritePng(folder + "/" + name + ".png", frame)) ++failures;
    };

    View view;
    view.results = &partial;
    view.card = CardState::Ready;
    view.levelDb = -71.3f;
    view.peakHoldDb = -58.0f;
    view.run = 3;
    view.screen = Screen::Intro;
    shot("1-intro", view);

    view.screen = Screen::Ready;
    view.step = 5;
    view.levelDb = -74.0f;
    view.peakHoldDb = -66.5f;
    shot("2-ready", view);
    view.step = 0;
    view.card = CardState::None;
    shot("2-ready-quiet-no-card", view);
    view.step = 15;
    view.card = CardState::Ready;
    shot("2-ready-music", view);

    view.screen = Screen::Recording;
    view.step = 5;
    view.elapsed = 1.7f;
    view.levelDb = -33.6f;
    view.peakHoldDb = -12.4f;
    shot("3-recording", view);

    view.screen = Screen::Result;
    view.step = 5;
    view.card = CardState::Saved;
    shot("4-result", view);
    view.step = 13;
    view.card = CardState::Saving;
    view.results = &complete;
    shot("4-result-clipped", view);

    view.screen = Screen::Table;
    view.results = &partial;
    view.canResume = true;
    view.fromBoot = true;
    view.tableFirst = 0;
    view.card = CardState::Ready;
    shot("5-table-from-boot", view);
    view.results = &complete;
    view.canResume = false;
    view.fromBoot = false;
    view.tableFirst = kStepCount - kTableRows;
    shot("5-table-done", view);
    view.card = CardState::None;
    view.tableFirst = 6;
    shot("5-table-done-no-card", view);

    if (MissingGlyphs() != 0)
    {
        std::printf("FAIL: %d characters drawn that the font lacks (tools/make_font.py)\n", MissingGlyphs());
        ++failures;
    }
    if (failures == 0) std::printf("screens: written to %s\n", folder.c_str());
    return failures == 0 ? 0 : 1;
}
