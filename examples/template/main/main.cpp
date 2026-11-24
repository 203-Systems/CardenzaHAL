// A starting point for a Cardenza firmware on CardenzaHAL: replace it with yours.
//
// - The screen shows the firmware's name and version and the 4 x 14 keys; a held key fills its cell and
//   lights its LED, the others glow faintly.
// - ok (enter) inverts the screen. HOME (the side key) is the system's: a short press opens the system menu,
//   HOME + space takes a screenshot (screenshot/screenshot.h); the firmware does nothing for either.
// - Keys: keyboard/legends.h says what each cap prints (Legend, Typed; the arrows are ; . , / and ok
//   confirms, esc goes back). Use it rather than a layout of your own.
// - The SD card: its files are in its own folder, /sdcard/Apps/<project name>/ (storage/app_dir.h,
//   docs/SD-CARD.md): Settings/app.cfg counts its starts, Logs/log.txt gets a line at each.
//
// - The screen goes out as the HAL's presenter does it (display/presenter.h, docs/DISPLAY.md): the loop runs
//   every 10 ms and draws every other time (50 fps), PresentChanged sends only what changed, and once a minute
//   the log says what the screen cost.
//
// It reaches the hardware only through the HAL's public headers, so the same source builds for the board
// (idf.py build) and for the browser (CardenzaHAL tools/cardenza_web.py build). Keep CardenzaHAL::Init()
// first: its log lines are how a crash log, and the 203 community's checks, know the HAL.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>

#include <esp_app_desc.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "cardenzaHAL.h"
#include "keyboard/led_light.h"
#include "keyboard/legends.h"
#include "storage/app_dir.h"

namespace {

namespace Display = CardenzaHAL::Display;
namespace Keyboard = CardenzaHAL::Keyboard;
namespace Storage = CardenzaHAL::Storage;
namespace System = CardenzaHAL::System;

constexpr char kTag[] = "app";
constexpr int kColumns = 14;
constexpr int kRows = 4;
constexpr int kCellWidth = 16;
constexpr int kCellHeight = 22;
constexpr int kGridX = (Display::WIDTH - kColumns * kCellWidth) / 2;
constexpr int kGridY = 36;

constexpr uint16_t Rgb(int r, int g, int b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// The framebuffer: 64,800 bytes, static (not on the task's stack), 4-byte aligned for PresentChanged. A firmware
// short of internal RAM takes it from PSRAM instead: Display::AllocateFramebuffer().
alignas(4) uint16_t g_frame[Display::WIDTH * Display::HEIGHT];

void Fill(int x, int y, int width, int height, uint16_t colour)
{
    for (int row = y; row < y + height && row < Display::HEIGHT; ++row)
        for (int column = x; column < x + width && column < Display::WIDTH; ++column) g_frame[row * Display::WIDTH + column] = colour;
}

void Outline(int x, int y, int width, int height, uint16_t colour)
{
    Fill(x, y, width, 1, colour);
    Fill(x, y + height - 1, width, 1, colour);
    Fill(x, y, 1, height, colour);
    Fill(x + width - 1, y, 1, height, colour);
}

void Draw(const std::vector<Keyboard::Point2D_t>& keys, bool inverted, int versionLength)
{
    const uint16_t background = inverted ? Rgb(232, 232, 224) : Rgb(0, 0, 0);
    const uint16_t ink = inverted ? Rgb(0, 0, 0) : Rgb(232, 232, 224);
    const uint16_t faint = Rgb(58, 58, 54);
    Fill(0, 0, Display::WIDTH, Display::HEIGHT, background);
    // No font here (bring your own): a bar as long as the version string stands for the title.
    Fill(kGridX, 12, 6 * versionLength, 8, Rgb(255, 208, 48));
    for (int y = 0; y < kRows; ++y)
        for (int x = 0; x < kColumns; ++x)
        {
            const int left = kGridX + x * kCellWidth;
            const int top = kGridY + y * kCellHeight;
            bool held = false;
            for (const auto& key : keys) held |= key.x == x && key.y == y;
            if (held) Fill(left + 1, top + 1, kCellWidth - 2, kCellHeight - 2, ink);
            else Outline(left + 1, top + 1, kCellWidth - 2, kCellHeight - 2, faint);
        }
}

// ---- The SD card ----------------------------------------------------------------------------------------------
// Everything this firmware keeps is in its own folder, /sdcard/Apps/<project name>/ (Storage::AppDir; menuconfig
// CardenzaHAL > folder name chooses another): Settings/ for its settings, Logs/ for its logs, the rest as it likes.
// Never at the card's root and never in /sdcard/Launcher (the 203 Launcher's). The board's own settings (volume,
// brightness, language) are not files: system/settings.h. A firmware that kept its files elsewhere before moves
// them once, at the start, before reading any: Storage::MigrateAppDir("/sdcard/<its old folder>").
// The user pulls the power whenever: a file is never left half written.

// Reads `path`, or the copy WriteFile left when the power went between its last two steps.
std::string ReadFile(const std::string& path)
{
    std::string text;
    for (const std::string& source : {path, path + ".tmp"})
        if (std::FILE* file = std::fopen(source.c_str(), "r"))
        {
            char buffer[256];
            for (std::size_t read; (read = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) text.append(buffer, read);
            std::fclose(file);
            return text;
        }
    return text;
}

// The new contents go to <path>.tmp first; only then the old file goes and the new one takes its name (FAT does not
// rename over a file). A power cut leaves the old file, or the new one, or the new one as .tmp (ReadFile finds it).
bool WriteFile(const std::string& path, const std::string& text)
{
    const std::string temporary = path + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "w");
    if (file == nullptr) return false;
    const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    if (std::fclose(file) != 0 || !written) return false;
    std::remove(path.c_str());
    return std::rename(temporary.c_str(), path.c_str()) == 0;
}

void NoteStart(const esp_app_desc_t* app)
{
    // No card (or none that mounts): run without one; never wait for it or restart.
    if (!Storage::IsMounted() || !Storage::EnsureAppDir(Storage::kSettingsFolder) || !Storage::EnsureAppDir(Storage::kLogsFolder))
    {
        ESP_LOGI(kTag, "no SD card: nothing kept");
        return;
    }
    // Settings/app.cfg: key=value lines, read tolerantly (a missing file is the defaults).
    const std::string settings = Storage::AppPath("Settings/app.cfg");
    const std::string text = ReadFile(settings);
    const std::size_t at = text.find("starts=");
    const long starts = (at == std::string::npos ? 0 : std::strtol(text.c_str() + at + 7, nullptr, 10)) + 1;
    WriteFile(settings, "starts=" + std::to_string(starts) + "\n");
    // Logs/log.txt: a line a start; at 64 KB it becomes log.1.txt (the one before is dropped).
    const std::string log = Storage::AppPath("Logs/log.txt");
    struct stat info{};
    if (stat(log.c_str(), &info) == 0 && info.st_size > 64 * 1024)
    {
        const std::string older = Storage::AppPath("Logs/log.1.txt");
        std::remove(older.c_str());
        std::rename(log.c_str(), older.c_str());
    }
    if (std::FILE* file = std::fopen(log.c_str(), "a"))
    {
        std::fprintf(file, "%s %s started (%ld)\n", app->project_name, app->version, starts);
        std::fclose(file);
    }
    ESP_LOGI(kTag, "card folder %s, start %ld", Storage::AppDir().c_str(), starts);
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
    // The first line of every start says what is running: the app description's name and version
    // (version.txt). Keep the ELF of every build you give anyone: a backtrace is read with it.
    const esp_app_desc_t* app = esp_app_get_description();
    ESP_LOGI(kTag, "%s %s ready", app->project_name, app->version);
    NoteStart(app);

    // The screen's and the key lights' brightness and the volume are the board's settings (system/settings.h,
    // HOME > Settings): CardenzaHAL::Init applied them. Setting them here would override the user's choice.
    Keyboard::enableLED(true);

    bool inverted = false;
    bool okBefore = false;
    const int versionLength = static_cast<int>(std::strlen(app->version));
    // A tick every 10 ms (keys and lights), a frame every other tick (50 fps). Your loop's own work goes in
    // every tick; drawing only on the ticks that draw.
    Display::FramePacer pacer;
    for (;;)
    {
        Keyboard::updateKeyList();
        const auto& keys = Keyboard::keyList();
        const bool ok = Keyboard::isKeyPressing(Keyboard::kKeyOk);
        if (ok && !okBefore) inverted = !inverted;
        okBefore = ok;

        Keyboard::LedFrame leds{};
        leds.fill(Keyboard::Light::PerceivedToLevel(4));
        for (const auto& key : keys) leds[key.y * Keyboard::LED_COLUMNS + key.x] = Keyboard::Light::PerceivedToLevel(15);
        Keyboard::setLEDFrame(leds);

        if (pacer.DrawTick())
        {
            Draw(keys, inverted, versionLength);
            Display::PresentChanged(g_frame);  // only what changed (a whole frame is 64,800 bytes)
        }
        // What the screen costs, once a minute (Display::LastFrame() has the last frame's own numbers).
        if (pacer.Tick() % 6000 == 5999)
        {
            const Display::PresentTotals totals = Display::Totals();
            if (totals.frames > 0)
                ESP_LOGI(kTag, "screen: %u frames, %u us a frame (max %u), %u bytes a frame, %u late ticks",
                         static_cast<unsigned>(totals.frames), static_cast<unsigned>(totals.presentUs / totals.frames),
                         static_cast<unsigned>(totals.maxPresentUs), static_cast<unsigned>(totals.bytes / totals.frames),
                         static_cast<unsigned>(pacer.LateTicks()));
            Display::ResetTotals();
        }
        pacer.Wait();
    }
}
