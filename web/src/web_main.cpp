// The web target's entry: ESP-IDF's startup in short (the kernel, the heap, the "main" task that
// calls app_main), and the C interface the page (web/page/cardenza-sim.js) drives the simulator with.
//
// The page calls cardenza_web_run() from the browser's event loop and again after
// cardenza_web_next_delay() milliseconds (or at once after input); between calls it copies the
// screen, the key LEDs and the speaker's frames out, and puts keys, HOME, MIDI and the microphone in.

#include <emscripten/emscripten.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "drivers/audio_state.h"
#include "drivers/board_io.h"
#include "drivers/display_state.h"
#include "drivers/st7789.h"
#include "esp/heap_internal.h"
#include "kernel/kernel.h"
#include "platform.h"
#include "web/hal/web_keyboard.h"
#include "web/hal/web_usb.h"

#ifndef CARDENZA_HAL_VERSION
#define CARDENZA_HAL_VERSION "unknown"
#endif
#ifndef CARDENZA_WEB_PROJECT_NAME
#define CARDENZA_WEB_PROJECT_NAME "firmware"
#endif

extern "C" void app_main(void);

namespace cardenza_web::freertos {
void EnsureStarted();
}
namespace cardenza_web::esp {
void InstallIdleHooks();
}

namespace k = cardenza_web::kernel;

namespace {

bool g_halted = false;
esp_reset_reason_t g_resetReason = ESP_RST_POWERON;
bool g_cardPresent = true;
uint64_t g_cardBytes = 8ull * 1024 * 1024 * 1024;

EM_JS(double, js_audio_clock, (), {
    return Module.cardenzaAudioClock ? Module.cardenzaAudioClock() : performance.now() / 1000;
});
EM_JS(double, js_config, (const char* name, double fallback), {
    const config = Module.cardenzaConfig || {};
    const value = config[UTF8ToString(name)];
    return typeof value === 'number' ? value : (typeof value === 'boolean' ? (value ? 1 : 0) : fallback);
});
EM_JS(void, js_call, (const char* name), {
    const handler = Module[UTF8ToString(name)];
    if (typeof handler === 'function') handler();
});
EM_JS(void, js_restart, (int reason), {
    if (Module.cardenzaRestart) Module.cardenzaRestart(reason);
});
EM_JS(uint32_t, js_random, (), {
    const word = new Uint32Array(1);
    crypto.getRandomValues(word);
    return word[0];
});
EM_JS(void, js_ir, (int address, int command, int nec), {
    if (Module.cardenzaIr) Module.cardenzaIr(address, command, !!nec);
});
EM_JS(void, js_midi_out, (int b0, int b1, int b2, int b3), {
    if (Module.cardenzaMidiOut) Module.cardenzaMidiOut([b0, b1, b2, b3]);
});
EM_JS(void, js_app_report, (const uint8_t* data, int length), {
    if (Module.cardenzaAppReport) Module.cardenzaAppReport(HEAPU8.slice(data, data + length));
});
EM_JS(void, js_usb_started, (const char* product), {
    if (Module.cardenzaUsbStarted) Module.cardenzaUsbStarted(UTF8ToString(product));
});

void MainTask(void*)
{
    cardenza_web::platform::Booted();
    ESP_LOGI("main_task", "Calling app_main()");
    app_main();
    ESP_LOGI("main_task", "Returned from app_main()");
    vTaskDelete(nullptr);
}

struct DisplayFrame {
    uint32_t serial;
    uint32_t backlightPermille;
    uint32_t showing;
    uint16_t pixels[cardenza_web::display::St7789::kViewWidth * cardenza_web::display::St7789::kViewHeight];
};
DisplayFrame g_frame;
uint32_t g_frameChanges = 0xFFFFFFFF;
float g_frameBacklight = -1.0f;

constexpr int kAudioTake = 8192;
float g_audio[kAudioTake];
double g_audioPosition = 0;
float g_mic[4096];
uint8_t g_appIn[64];

}  // namespace

namespace cardenza_web::audio {
double ClockFrames() { return js_audio_clock() * kRate; }
}  // namespace cardenza_web::audio

namespace cardenza_web::platform {

void Restart(esp_reset_reason_t reason)
{
    std::printf("I (%u) cardenza-web: restart (%d)\n", static_cast<unsigned>(esp_log_timestamp()), static_cast<int>(reason));
    std::fflush(stdout);
    g_halted = true;
    js_restart(static_cast<int>(reason));
    // The page starts a new instance; this one only has to stop.
    if (k::InTask())
        for (;;) k::Block(nullptr, k::kForever);
    std::abort();
}

esp_reset_reason_t ResetReason() { return g_resetReason; }
uint32_t Random32() { return js_random(); }
bool CardPresent() { return g_cardPresent; }
uint64_t CardBytes() { return g_cardBytes; }
void FilesChanged() { js_call("cardenzaFilesChanged"); }
void IrSent(uint8_t address, uint8_t command, bool nec) { js_ir(address, command, nec ? 1 : 0); }
void MidiOut(const uint8_t packet[4]) { js_midi_out(packet[0], packet[1], packet[2], packet[3]); }
void AppReport(const uint8_t* data, std::size_t length) { js_app_report(data, static_cast<int>(length)); }
void UsbStarted(const char* product) { js_usb_started(product); }
void Booted() { js_call("cardenzaBooted"); }

}  // namespace cardenza_web::platform

int main()
{
    g_resetReason = static_cast<esp_reset_reason_t>(static_cast<int>(js_config("resetReason", ESP_RST_POWERON)));
    g_cardPresent = js_config("card", 1) != 0;
    g_cardBytes = static_cast<uint64_t>(js_config("cardMegabytes", 8192)) * 1024 * 1024;
    cardenza_web::freertos::EnsureStarted();
    cardenza_web::esp::InstallIdleHooks();
    std::printf("I (%u) cpu_start: CardenzaHAL %s web target: %s on a simulated ESP32-S3 (2 cores, ESP-IDF API %s)\n",
                static_cast<unsigned>(esp_log_timestamp()), CARDENZA_HAL_VERSION, CARDENZA_WEB_PROJECT_NAME, IDF_VER);
    cardenza_web::heap::PrintSummary();
    TaskHandle_t main = nullptr;
    xTaskCreatePinnedToCore(MainTask, "main", ESP_TASK_MAIN_STACK, nullptr, ESP_TASK_MAIN_PRIO, &main,
                            ESP_TASK_MAIN_CORE == 0x7FFFFFFF ? tskNO_AFFINITY : ESP_TASK_MAIN_CORE);
    // The page runs the scheduler from now on (cardenza_web_run).
    return 0;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE void cardenza_web_run(double budgetMs)
{
    if (g_halted) return;
    k::RunFor(static_cast<uint64_t>(budgetMs * 1000.0));
}

// Milliseconds until the scheduler has something to do: 0 now, -1 nothing until input.
EMSCRIPTEN_KEEPALIVE double cardenza_web_next_delay()
{
    if (g_halted) return -1;
    const int64_t micros = k::NextDelayUs();
    return micros < 0 ? -1.0 : static_cast<double>(micros) / 1000.0;
}

// The glass as the eye sees it (RGB565, 240 x 135, landscape) and the backlight's light.
EMSCRIPTEN_KEEPALIVE const DisplayFrame* cardenza_web_display()
{
    auto& controller = cardenza_web::display::Controller();
    const float backlight = cardenza_web::display::Backlight();
    if (controller.Changes() != g_frameChanges || backlight != g_frameBacklight)
    {
        g_frameChanges = controller.Changes();
        g_frameBacklight = backlight;
        controller.RenderView(g_frame.pixels);
        g_frame.backlightPermille = static_cast<uint32_t>(backlight * 1000.0f + 0.5f);
        g_frame.showing = controller.Showing() ? 1 : 0;
        ++g_frame.serial;
    }
    return &g_frame;
}

EMSCRIPTEN_KEEPALIVE const cardenza_web::keyboard::LedSnapshot* cardenza_web_leds() { return &cardenza_web::keyboard::Leds(); }

// A key of the 4 x 14 matrix (x 0..13, y 0..3) held or let go.
EMSCRIPTEN_KEEPALIVE void cardenza_web_key(int x, int y, int down) { cardenza_web::keyboard::SetKey(x, y, down != 0); }
EMSCRIPTEN_KEEPALIVE void cardenza_web_release_keys() { cardenza_web::keyboard::ReleaseAll(); }
EMSCRIPTEN_KEEPALIVE void cardenza_web_home(int down) { cardenza_web::board::SetHome(down != 0); }

// The speaker: the next frames (mono, -1..1, the codec's volume applied) at the position
// cardenza_web_audio_position() gives, in frames of the audio clock at 44.1 kHz.
EMSCRIPTEN_KEEPALIVE int cardenza_web_audio_take()
{
    return static_cast<int>(cardenza_web::audio::TakeSpeaker(g_audio, kAudioTake, &g_audioPosition));
}
EMSCRIPTEN_KEEPALIVE float* cardenza_web_audio_buffer() { return g_audio; }
EMSCRIPTEN_KEEPALIVE double cardenza_web_audio_position() { return g_audioPosition; }

// The microphone: the page writes frames into the buffer, then says where they belong.
EMSCRIPTEN_KEEPALIVE float* cardenza_web_mic_buffer() { return g_mic; }
EMSCRIPTEN_KEEPALIVE void cardenza_web_mic_push(double position, int count)
{
    cardenza_web::audio::PushMicrophone(position, g_mic, static_cast<std::size_t>(count < 4096 ? count : 4096));
}
EMSCRIPTEN_KEEPALIVE int cardenza_web_mic_wanted() { return cardenza_web::audio::MicrophoneWanted() ? 1 : 0; }

// USB from the host: a USB-MIDI packet, an app report (the page writes it into the buffer first).
EMSCRIPTEN_KEEPALIVE void cardenza_web_midi_in(int b0, int b1, int b2, int b3)
{
    const uint8_t packet[4] = {static_cast<uint8_t>(b0), static_cast<uint8_t>(b1), static_cast<uint8_t>(b2), static_cast<uint8_t>(b3)};
    cardenza_web::usb::MidiFromHost(packet);
}
EMSCRIPTEN_KEEPALIVE uint8_t* cardenza_web_app_buffer() { return g_appIn; }
EMSCRIPTEN_KEEPALIVE void cardenza_web_app_in(int length)
{
    cardenza_web::usb::AppFromHost(g_appIn, static_cast<std::size_t>(length < 64 ? length : 64));
}

// vTaskList, for the page's diagnostics.
EMSCRIPTEN_KEEPALIVE const char* cardenza_web_tasks()
{
    static char text[2048];
    vTaskList(text);
    return text;
}

}  // extern "C"
