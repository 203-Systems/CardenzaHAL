// The board's settings (system/settings.h) on the web target: the same values, ranges and effects (the
// output's level, the backlight, the key lights' master level, the microphone's gain), kept for the page's
// life only: a simulator has no Launcher and no flash of the board to share them with, and no system menu.
#include "system/settings.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#include "audio/audio.h"
#include "display/internal/system_brightness.h"
#include "keyboard/keyboard.h"
#include "system/internal/settings_hooks.h"
#include "system/settings_logic.h"

namespace CardenzaHAL::Settings {

namespace {

using detail::Index;

std::mutex g_mutex;
detail::Values g_values = detail::Defaults();
uint32_t g_set = 0;
std::atomic<int32_t> g_micGain{4096};
Listener g_listener = nullptr;
void* g_listenerContext = nullptr;

void ApplyOne(Id id, int value)
{
    switch (id)
    {
        case Id::Volume: (void)Audio::SetVolume(detail::CodecPercent(value)); break;
        case Id::ScreenBrightness: Display::detail::SetSystemBrightness(value); break;
        case Id::KeyBrightness: Keyboard::setLEDGlobalBrightness(static_cast<uint8_t>(value)); break;
        case Id::MicGain: g_micGain.store(detail::MicGainQ12(value)); break;
        default: break;
    }
}

void Changed(uint32_t changed, const detail::Values& values)
{
    Listener listener;
    void* context;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        listener = g_listener;
        context = g_listenerContext;
    }
    for (int i = 0; i < kCount; ++i)
        if ((changed >> i) & 1)
        {
            ApplyOne(detail::IdAt(i), values[i]);
            if (listener != nullptr) listener(detail::IdAt(i), values[i], context);
        }
}

}  // namespace

const Info& Describe(Id id)
{
    return detail::kInfo[std::clamp(Index(id), 0, kCount - 1)];
}

int Get(Id id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_values[std::clamp(Index(id), 0, kCount - 1)];
}

int Set(Id id, int value)
{
    if (Index(id) < 0 || Index(id) >= kCount) return 0;
    uint32_t changed;
    detail::Values values;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        changed = detail::Apply(g_values, id, value);
        g_set |= 1u << Index(id);
        values = g_values;
    }
    Changed(changed, values);
    return values[Index(id)];
}

void Reset()
{
    uint32_t changed;
    detail::Values values;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        changed = detail::ApplyReset(g_values);
        values = g_values;
    }
    Changed(changed, values);
}

bool Stored(Id id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return (g_set >> Index(id)) & 1;
}

bool Flush()
{
    return true;
}

bool Saved()
{
    return true;
}

void SetListener(Listener listener, void* context)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_listener = listener;
    g_listenerContext = context;
}

namespace detail {

void Load()
{
    (void)Audio::SetVolume(CodecPercent(Settings::Get(Id::Volume)));
    Display::detail::SetSystemBrightness(Settings::Get(Id::ScreenBrightness));
}

void ApplyAfterInit()
{
    Load();
    Keyboard::setLEDGlobalBrightness(static_cast<uint8_t>(Settings::Get(Id::KeyBrightness)));
}

void MenuClosed() {}

int32_t MicGainQ12Now()
{
    return g_micGain.load(std::memory_order_relaxed);
}

}  // namespace detail

}  // namespace CardenzaHAL::Settings
