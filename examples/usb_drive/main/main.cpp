// The SD card as a USB drive (usb/drive.h). Press any key: the card goes to the computer (a "Cardenza SD" disk
// appears); press again: it comes back to this firmware. Ejecting the disk on the computer, unplugging the cable or
// putting the computer to sleep gives it back too. The screen: grey, the firmware has the card; amber, lending or
// giving back; green, the computer has it (a bar grows with what it read and wrote); red, it failed (see the log).
// Each time the card comes back the firmware writes a line to its own folder, to show it has the card again.
#include <cstdint>
#include <cstdio>
#include <string>

#include <esp_log.h>

#include "cardenzaHAL.h"
#include "keyboard/led_light.h"
#include "storage/app_dir.h"
#include "usb/drive.h"

namespace {

namespace Display = CardenzaHAL::Display;
namespace Drive = CardenzaHAL::USB::Drive;
namespace Keyboard = CardenzaHAL::Keyboard;
namespace Storage = CardenzaHAL::Storage;

constexpr char kTag[] = "usb_drive";

constexpr uint16_t Rgb(int r, int g, int b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

alignas(4) uint16_t g_frame[Display::WIDTH * Display::HEIGHT];

void Fill(int x, int y, int width, int height, uint16_t colour)
{
    for (int row = y; row < y + height && row < Display::HEIGHT; ++row)
        for (int column = x; column < x + width && column < Display::WIDTH; ++column) g_frame[row * Display::WIDTH + column] = colour;
}

// The card is the firmware's again: a line in its own folder.
void NoteBack(const Drive::Status& status)
{
    if (!Storage::IsMounted() || !Storage::EnsureAppDir(Storage::kLogsFolder)) return;
    const std::string log = Storage::AppPath("Logs/log.txt");
    if (std::FILE* file = std::fopen(log.c_str(), "a"))
    {
        std::fprintf(file, "card back (%s): computer read %llu, wrote %llu bytes\n", Drive::ReasonName(status.reason),
                     static_cast<unsigned long long>(status.bytesRead), static_cast<unsigned long long>(status.bytesWritten));
        std::fclose(file);
    }
}

// From the drive's task. On Starting this firmware would close its files on the card (it keeps none open).
void Changed(const Drive::Status& status)
{
    ESP_LOGI(kTag, "drive %s (%s), card %s", Drive::StateName(status.state), Drive::ReasonName(status.reason),
             Storage::LentToComputer() ? "lent" : (Storage::IsMounted() ? "mounted" : "not mounted"));
}

void Draw(const Drive::Status& status)
{
    uint16_t background = Rgb(40, 40, 38);
    switch (status.state)
    {
        case Drive::State::Off: background = Rgb(40, 40, 38); break;
        case Drive::State::Starting:
        case Drive::State::Returning: background = Rgb(150, 100, 20); break;
        case Drive::State::Shared: background = Rgb(20, 110, 50); break;
        case Drive::State::Failed: background = Rgb(140, 30, 30); break;
    }
    Fill(0, 0, Display::WIDTH, Display::HEIGHT, background);
    // A bar of what the computer moved: a pixel every 64 KB, wrapping.
    const uint64_t moved = status.bytesRead + status.bytesWritten;
    const int length = static_cast<int>((moved / 65536) % Display::WIDTH);
    Fill(0, Display::HEIGHT - 12, length, 12, Rgb(232, 232, 224));
    if (status.ioErrors != 0) Fill(0, 0, Display::WIDTH, 6, Rgb(255, 52, 68));
}

}  // namespace

extern "C" void app_main()
{
    if (!CardenzaHAL::Init()) ESP_LOGE(kTag, "HAL init failed");
    Drive::SetListener(Changed);
    Keyboard::enableLED(true);

    bool keyBefore = false;
    Drive::State before = Drive::State::Off;
    Display::FramePacer pacer;
    for (;;)
    {
        Keyboard::updateKeyList();
        const bool key = !Keyboard::keyList().empty();
        const Drive::Status status = Drive::GetStatus();
        if (key && !keyBefore)
        {
            if (status.state == Drive::State::Shared) Drive::Stop(0);
            else if (status.state == Drive::State::Off || status.state == Drive::State::Failed) Drive::Share(0);
        }
        keyBefore = key;
        if (before != Drive::State::Off && status.state == Drive::State::Off) NoteBack(status);
        before = status.state;

        Keyboard::LedFrame leds{};
        leds.fill(Keyboard::Light::PerceivedToLevel(status.state == Drive::State::Shared ? 15 : 4));
        Keyboard::setLEDFrame(leds);
        if (pacer.DrawTick())
        {
            Draw(status);
            Display::PresentChanged(g_frame);
        }
        if (pacer.Tick() % 200 == 199 && status.state == Drive::State::Shared)
            ESP_LOGI(kTag, "computer read %llu, wrote %llu bytes, %u errors", static_cast<unsigned long long>(status.bytesRead),
                     static_cast<unsigned long long>(status.bytesWritten), static_cast<unsigned>(status.ioErrors));
        pacer.Wait();
    }
}
