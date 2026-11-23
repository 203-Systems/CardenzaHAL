// mic-check: how loud the board's microphone is, raw, at several distances and loudnesses, to choose the
// default microphone gain from measurements (the board's setting, Settings::Id::MicGain).
//
// A guided test on the screen: 5 s of quiet (the noise floor), then speech soft / normal / loud at 10 cm,
// 30 cm, 1 m and 2 m, claps at 30 cm and 1 m, music from a phone at 30 cm. Each step: ok records 5 s (a
// countdown and the live level with its peak held), then the take's numbers; ok goes on, esc records it
// again, / (right) skips a step, , (left) goes back one. At the end a table of every step (; . scroll).
//
// What is measured is the HAL's microphone as it delivers it (Mic::InputChannel: PDM, 44,100 Hz, mono,
// signed 16-bit), never passed through Mic::ApplyGain: the board's own gain setting is reported, not
// applied. The speaker stays stopped (its codec in standby: the microphone's clock is the speaker's WS pin,
// GPIO43). Levels are dBFS relative to 32768 (levels.h).
//
// Results:
// - the log (the chip's USB-Serial-JTAG console; USB::Init is never called): one line a take,
//   "MICTEST take=6 run=2 dist=30cm level=normal rms=-31.2 peak=-9.8 p95=-24.0 ...", and once the test is
//   done the whole table ("MICTEST table ...", a "MICTEST row ..." a step, "MICTEST end ...") every 15 s;
// - RTC memory (RTC_NOINIT_ATTR, a magic and a checksum): a reset, such as opening the serial port, keeps
//   them; the board starts on the table and prints it;
// - the SD card, when one is mounted: /sdcard/Apps/MicCheck/run-NN/ (every HAL firmware's files are under
//   Apps/<AppName>/, the Launcher's under Launcher/) with each take as a WAV (the raw samples),
//   results.csv and info.txt. Without a card nothing is written.
//
// Tasks: the microphone's reader (core 1, above the UI) measures every block and fills the take; the UI runs
// in app_main at 50 fps; a writer below them puts takes on the card. The pure parts (levels, results, the
// screens and their font) are tested on the computer: test/.

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <type_traits>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_app_desc.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "cardenzaHAL.h"
#include "keyboard/legends.h"
#include "keyboard/led_light.h"

#include "canvas.h"
#include "levels.h"
#include "plan.h"
#include "results.h"
#include "screens.h"

namespace {

using namespace MicCheck;
namespace Audio = CardenzaHAL::Audio;
namespace Display = CardenzaHAL::Display;
namespace Keyboard = CardenzaHAL::Keyboard;
namespace Mic = CardenzaHAL::Mic;
namespace Settings = CardenzaHAL::Settings;
namespace Storage = CardenzaHAL::Storage;

static_assert(kWidth == Display::WIDTH && kHeight == Display::HEIGHT, "the screens are the panel's size");

constexpr char kTag[] = "mic-check";
constexpr char kApps[] = "/sdcard/Apps";
constexpr char kFolder[] = "/sdcard/Apps/MicCheck";
constexpr std::size_t kBlockSamples = 512;      // what the reader asks of the driver at a time (11.6 ms)
constexpr int64_t kWarmUpUs = 500 * 1000;       // the PDM filter settles before a take may start
constexpr int64_t kTablePrintUs = 15 * 1000 * 1000;
constexpr int64_t kPeakHoldUs = 1500 * 1000;
constexpr float kPeakFallDbPerSecond = 30.0f;
constexpr uint8_t kKeySub = 28;                 // \ under del: ADD / SUB in CardenzaSynth, the table scrolls with them too

// The results, kept through resets (not a power cut) in RTC slow memory. The 203 Launcher's own image puts 32
// bytes at its start whenever the Launcher runs (.rtc.force_slow): the results sit 512 bytes in, so a trip
// through the Launcher's home screen leaves them too; the checksum catches whatever else writes there.
// Changed only by the UI, under g_resultsLock.
struct RtcSpace
{
    uint8_t leftToOthers[512];
    Results results;
};
static_assert(std::is_trivially_default_constructible_v<RtcSpace>);
RTC_NOINIT_ATTR RtcSpace g_rtcSpace;
Results& g_rtc = g_rtcSpace.results;
SemaphoreHandle_t g_resultsLock = nullptr;

// The microphone's reader and the UI.
enum class Command : uint8_t { Start, Cancel };
QueueHandle_t g_commands = nullptr;
std::atomic<int32_t> g_levelCentiDb{static_cast<int32_t>(kFloorDb * 100)};  // the last ~58 ms
std::atomic<uint32_t> g_peakSinceRead{0};                                   // largest |sample| since the UI read it
std::atomic<uint32_t> g_takeSamples{0};                                     // of the take running
std::atomic<int> g_takeState{0};                                            // 0 none, 1 running, 2 done
Levels g_takeLevels;                                                        // written before g_takeState = 2
uint32_t g_takeOverflows = 0;
int16_t* g_takeBuffer = nullptr;                                            // PSRAM, kTakeSamples; nullptr: no WAVs
volatile uint32_t g_dropped = 0;  // DMA buffers the driver dropped (its interrupt is the one writer)

// The card's writer.
TaskHandle_t g_writer = nullptr;
std::atomic<bool> g_card{false};          // mounted at the start and no write failed
std::atomic<bool> g_cardFailed{false};
std::atomic<bool> g_saving{false};        // a take's WAV is being written: the buffer is the writer's
std::atomic<int> g_wavStep{-1};           // the step g_takeBuffer holds, for the writer
std::atomic<int> g_savedStep{-1};         // the step whose WAV is on the card (this run)
std::atomic<uint32_t> g_wavRun{0};

alignas(4) uint16_t g_frame[kWidth * kHeight];

bool IRAM_ATTR OnReceiveOverflow(i2s_chan_handle_t, i2s_event_data_t*, void*)
{
    g_dropped = g_dropped + 1;
    return false;
}

// ---- The microphone ----------------------------------------------------------------------------------

void ReaderTask(void* argument)
{
    auto* rx = static_cast<i2s_chan_handle_t>(argument);
    static int16_t block[kBlockSamples];
    static TakeAnalyzer analyzer;
    const int64_t started = esp_timer_get_time();
    bool running = false;
    uint32_t filled = 0;
    uint32_t droppedAtStart = 0;
    // The meter's window: blocks until 50 ms or more.
    int64_t meterSum = 0;
    uint64_t meterSquares = 0;
    uint32_t meterCount = 0;
    for (;;)
    {
        std::size_t bytes = 0;
        if (i2s_channel_read(rx, block, sizeof(block), &bytes, portMAX_DELAY) != ESP_OK || bytes == 0) continue;
        const std::size_t count = bytes / sizeof(int16_t);

        uint32_t peak = 0;
        for (std::size_t index = 0; index < count; ++index)
        {
            const int32_t sample = block[index];
            meterSum += sample;
            meterSquares += static_cast<uint32_t>(sample * sample);
            const uint32_t magnitude = static_cast<uint32_t>(sample < 0 ? -sample : sample);
            if (magnitude > peak) peak = magnitude;
        }
        meterCount += count;
        if (meterCount >= kWindowSamples)
        {
            const double mean = static_cast<double>(meterSum) / meterCount;
            const float db = PowerToDb(static_cast<double>(meterSquares) / meterCount - mean * mean);
            g_levelCentiDb.store(static_cast<int32_t>(db * 100.0f), std::memory_order_relaxed);
            meterSum = 0;
            meterSquares = 0;
            meterCount = 0;
        }
        if (peak > g_peakSinceRead.load(std::memory_order_relaxed)) g_peakSinceRead.store(peak, std::memory_order_relaxed);

        Command command;
        if (esp_timer_get_time() - started >= kWarmUpUs && xQueueReceive(g_commands, &command, 0) == pdTRUE)
        {
            if (command == Command::Start)
            {
                analyzer.Reset();
                filled = 0;
                droppedAtStart = g_dropped;
                running = true;
                g_takeSamples.store(0, std::memory_order_relaxed);
                g_takeState.store(1, std::memory_order_release);
            }
            else
            {
                running = false;
                g_takeState.store(0, std::memory_order_release);
            }
        }
        if (!running) continue;
        const std::size_t taken = analyzer.Add(block, count);
        if (g_takeBuffer != nullptr) std::memcpy(g_takeBuffer + filled, block, taken * sizeof(int16_t));
        filled += taken;
        g_takeSamples.store(filled, std::memory_order_relaxed);
        if (analyzer.Full())
        {
            g_takeLevels = analyzer.Finish();
            g_takeOverflows = g_dropped - droppedAtStart;
            running = false;
            g_takeState.store(2, std::memory_order_release);
        }
    }
}

bool StartMicrophone()
{
    // The speaker off: its TX channel stays disabled (Audio::Init leaves it so, and nothing here enables it)
    // and the codec in standby; then the microphone takes GPIO43 (README, "Microphone and shared audio pins").
    if (!Audio::StandbyCodec()) ESP_LOGW(kTag, "codec standby failed (the speaker may click; the measurement does not depend on it)");
    if (!Mic::Init())
    {
        ESP_LOGE(kTag, "Mic::Init failed");
        return false;
    }
    i2s_chan_handle_t rx = Mic::InputChannel();
    if (rx == nullptr)
    {
        ESP_LOGE(kTag, "no microphone channel");
        return false;
    }
    i2s_event_callbacks_t callbacks = {};
    callbacks.on_recv_q_ovf = OnReceiveOverflow;
    if (i2s_channel_register_event_callback(rx, &callbacks, nullptr) != ESP_OK) ESP_LOGW(kTag, "no overflow counter");
    if (!Mic::RestoreInputPins() || i2s_channel_enable(rx) != ESP_OK)
    {
        ESP_LOGE(kTag, "microphone did not start");
        return false;
    }
    g_commands = xQueueCreate(4, sizeof(Command));
    xTaskCreatePinnedToCore(ReaderTask, "mic_reader", 6144, rx, 10, nullptr, 1);
    return true;
}

// ---- The card -----------------------------------------------------------------------------------------

void CardFailed(const char* what, const char* path)
{
    ESP_LOGE(kTag, "card: %s %s failed (errno %d): no more writes this run", what, path, errno);
    Storage::CheckAfterIoError();
    g_cardFailed.store(true);
    g_card.store(false);
}

void RunFolder(char* out, std::size_t size, uint32_t run) { std::snprintf(out, size, "%s/run-%02u", kFolder, static_cast<unsigned>(run)); }

// The largest run-NN in the card's folder (0: none).
uint32_t LastRunOnCard()
{
    DIR* dir = opendir(kFolder);
    if (dir == nullptr) return 0;
    uint32_t last = 0;
    while (dirent* entry = readdir(dir))
    {
        unsigned run = 0;
        if (std::sscanf(entry->d_name, "run-%u", &run) == 1 && run > last) last = run;
    }
    closedir(dir);
    return last;
}

bool EnsureRunFolder(uint32_t run)
{
    char path[64];
    RunFolder(path, sizeof(path), run);
    struct stat info;
    if (stat(path, &info) == 0) return true;
    for (const char* parent : {kApps, kFolder})
        if (mkdir(parent, 0775) != 0 && errno != EEXIST)
        {
            CardFailed("mkdir", parent);
            return false;
        }
    if (mkdir(path, 0775) != 0 && errno != EEXIST)
    {
        CardFailed("mkdir", path);
        return false;
    }
    char file[96];
    std::snprintf(file, sizeof(file), "%s/info.txt", path);
    FILE* out = std::fopen(file, "w");
    if (out == nullptr)
    {
        CardFailed("open", file);
        return false;
    }
    const esp_app_desc_t* app = esp_app_get_description();
    std::fprintf(out,
                 "%s %s (ESP-IDF %s)\nmicrophone: PDM, %u Hz, mono, signed 16-bit, raw (no gain applied)\n"
                 "board mic_gain setting at the start of this run: %d dB (not applied)\n"
                 "levels: dBFS relative to 32768 (a full-scale sine has an RMS of -3.0 dBFS)\n"
                 "each take: %u s, %u samples; rms with its DC removed; p95 / p10 of the 50 ms levels; "
                 "loud1s the loudest second; clip = samples with |x| >= 32767\n",
                 app->project_name, app->version, app->idf_ver, static_cast<unsigned>(kSampleRate),
                 Settings::Get(Settings::Id::MicGain), static_cast<unsigned>(kTakeSeconds),
                 static_cast<unsigned>(kTakeSamples));
    if (std::fclose(out) != 0)
    {
        CardFailed("write", file);
        return false;
    }
    return true;
}

void Le16(uint8_t* at, uint32_t value)
{
    at[0] = static_cast<uint8_t>(value);
    at[1] = static_cast<uint8_t>(value >> 8);
}

void Le32(uint8_t* at, uint32_t value)
{
    Le16(at, value);
    Le16(at + 2, value >> 16);
}

// The take in g_takeBuffer as a 16-bit mono WAV. The SD card's DMA cannot read PSRAM (the driver would go
// a sector a command): the samples go through an internal buffer.
bool WriteWav(uint32_t run, int step)
{
    static uint8_t chunk[8192];
    char path[96];
    char folder[64];
    RunFolder(folder, sizeof(folder), run);
    std::snprintf(path, sizeof(path), "%s/%02d-%s-%s.wav", folder, step + 1, kPlan[step].distance[0] == '-' ? "none" : kPlan[step].distance,
                  SoundKey(kPlan[step].sound));
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd < 0)
    {
        CardFailed("open", path);
        return false;
    }
    const uint32_t dataBytes = kTakeSamples * sizeof(int16_t);
    uint8_t header[44];
    std::memcpy(header, "RIFF", 4);
    Le32(header + 4, 36 + dataBytes);
    std::memcpy(header + 8, "WAVEfmt ", 8);
    Le32(header + 16, 16);
    Le16(header + 20, 1);  // PCM
    Le16(header + 22, 1);  // mono
    Le32(header + 24, kSampleRate);
    Le32(header + 28, kSampleRate * 2);
    Le16(header + 32, 2);
    Le16(header + 34, 16);
    std::memcpy(header + 36, "data", 4);
    Le32(header + 40, dataBytes);
    bool ok = write(fd, header, sizeof(header)) == static_cast<ssize_t>(sizeof(header));
    const auto* samples = reinterpret_cast<const uint8_t*>(g_takeBuffer);
    for (uint32_t at = 0; ok && at < dataBytes; at += sizeof(chunk))
    {
        const uint32_t length = dataBytes - at < sizeof(chunk) ? dataBytes - at : sizeof(chunk);
        std::memcpy(chunk, samples + at, length);
        ok = write(fd, chunk, length) == static_cast<ssize_t>(length);
    }
    if (close(fd) != 0) ok = false;
    if (!ok)
    {
        CardFailed("write", path);
        return false;
    }
    ESP_LOGI(kTag, "card: %s", path);
    return true;
}

bool WriteCsv(const Results& results)
{
    char path[96];
    char folder[64];
    RunFolder(folder, sizeof(folder), results.run);
    std::snprintf(path, sizeof(path), "%s/results.csv", folder);
    FILE* out = std::fopen(path, "w");
    if (out == nullptr)
    {
        CardFailed("open", path);
        return false;
    }
    std::fprintf(out, "take,run,dist,level,state,rms_dbfs,peak_dbfs,p95_dbfs,p10_dbfs,loud1s_dbfs,clipped,full_scale_pct,"
                      "dc_lsb,overflows,samples,rate,bits\n");
    for (int step = 0; step < kStepCount; ++step)
    {
        const TakeRecord& take = results.takes[step];
        const Levels& l = take.levels;
        const char* state = take.state == kMeasured ? "measured" : take.state == kSkipped ? "skipped" : "open";
        if (take.state != kMeasured)
        {
            std::fprintf(out, "%d,%u,%s,%s,%s,,,,,,,,,,,%u,%u\n", step + 1, static_cast<unsigned>(results.run),
                         kPlan[step].distance, SoundKey(kPlan[step].sound), state, static_cast<unsigned>(results.sampleRate),
                         static_cast<unsigned>(results.bits));
            continue;
        }
        std::fprintf(out, "%d,%u,%s,%s,%s,%.2f,%.2f,%.2f,%.2f,%.2f,%u,%.2f,%.2f,%u,%u,%u,%u\n", step + 1,
                     static_cast<unsigned>(results.run), kPlan[step].distance, SoundKey(kPlan[step].sound), state, l.rms,
                     l.peak, l.p95, l.p10, l.loud1s, static_cast<unsigned>(l.clipped), l.fullScale, l.dc,
                     static_cast<unsigned>(take.overflows), static_cast<unsigned>(l.samples),
                     static_cast<unsigned>(results.sampleRate), static_cast<unsigned>(results.bits));
    }
    if (std::fclose(out) != 0)
    {
        CardFailed("write", path);
        return false;
    }
    return true;
}

// Woken by the UI: the take in the buffer (g_saving), and results.csv from the results as they are now.
void WriterTask(void*)
{
    static Results copy;
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // A take asked for after this read wakes the writer again (the notification counts).
        const bool wav = g_saving.load();
        xSemaphoreTake(g_resultsLock, portMAX_DELAY);
        copy = g_rtc;
        xSemaphoreGive(g_resultsLock);
        if (g_card.load() && Intact(copy) && copy.run != 0 && EnsureRunFolder(copy.run))
        {
            const int step = g_wavStep.load();
            if (wav && step >= 0 && g_takeBuffer != nullptr && g_wavRun.load() == copy.run && WriteWav(copy.run, step))
                g_savedStep.store(step);
            if (g_card.load()) WriteCsv(copy);
        }
        if (wav) g_saving.store(false);
    }
}

// The writer finds out itself whether the card is still there (and lets go of the buffer if not).
void WakeWriter()
{
    if (g_writer != nullptr) xTaskNotifyGive(g_writer);
}

// ---- The UI -------------------------------------------------------------------------------------------

struct Ui
{
    View view;
    bool haveResults = false;       // g_rtc holds a run
    bool startPending = false;      // ok pressed while the writer had the buffer
    bool tablePrinting = false;     // the test is over: the table goes to the log every 15 s
    int64_t lastTablePrint = 0;
    float peakHold = kFloorDb;
    int64_t peakHoldSince = 0;
    int64_t lastFrame = 0;
};

void LogTake(int step)
{
    char line[256];
    FormatLine(line, sizeof(line), "", g_rtc, step);
    std::printf("%s\n", line);
}

void PrintTable()
{
    char line[256];
    FormatTableHeader(line, sizeof(line), g_rtc, Settings::Get(Settings::Id::MicGain));
    std::printf("%s\n", line);
    for (int step = 0; step < kStepCount; ++step)
    {
        FormatLine(line, sizeof(line), "row", g_rtc, step);
        std::printf("%s\n", line);
    }
    std::printf("MICTEST end run=%u\n", static_cast<unsigned>(g_rtc.run));
}

CardState CardNow(const Ui& ui)
{
    if (g_cardFailed.load()) return CardState::Failed;
    if (!g_card.load()) return CardState::None;
    if (g_saving.load()) return CardState::Saving;
    if (g_savedStep.load() == ui.view.step && g_wavRun.load() == g_rtc.run) return CardState::Saved;
    return CardState::Ready;
}

void ShowTable(Ui& ui, bool fromBoot)
{
    ui.view.screen = Screen::Table;
    ui.view.fromBoot = fromBoot;
    ui.view.tableFirst = 0;
    ui.view.canResume = FirstOpenStep(g_rtc) < kStepCount;
    ui.tablePrinting = true;
    ui.lastTablePrint = esp_timer_get_time();
    PrintTable();
}

void GoToStep(Ui& ui, int step)
{
    ui.view.screen = Screen::Ready;
    ui.view.step = step;
    ui.startPending = false;
    ui.tablePrinting = false;
}

void NewRun(Ui& ui)
{
    uint32_t run = ui.haveResults ? g_rtc.run : 0;
    if (g_card.load())
    {
        const uint32_t onCard = LastRunOnCard();
        if (onCard > run) run = onCard;
    }
    ++run;
    xSemaphoreTake(g_resultsLock, portMAX_DELAY);
    ClearResults(g_rtc, run);
    xSemaphoreGive(g_resultsLock);
    ui.haveResults = true;
    g_savedStep.store(-1);
    ESP_LOGI(kTag, "run %u: %d steps of %u s%s", static_cast<unsigned>(run), kStepCount, static_cast<unsigned>(kTakeSeconds),
             g_card.load() ? ", takes to the card" : ", no card: the log only");
    WakeWriter();  // the run's folder, info.txt and an empty results.csv
    GoToStep(ui, 0);
}

void StartTake(Ui& ui)
{
    if (g_commands == nullptr)
    {
        ESP_LOGE(kTag, "no microphone: nothing to record (see the start's MICTEST boot line)");
        return;
    }
    if (g_saving.load())
    {
        ui.startPending = true;
        return;
    }
    ui.startPending = false;
    g_takeState.store(0);
    g_takeSamples.store(0);
    const Command command = Command::Start;
    xQueueSend(g_commands, &command, 0);
    ui.view.screen = Screen::Recording;
    ui.view.elapsed = 0.0f;
    ESP_LOGI(kTag, "take %d: %s %s", ui.view.step + 1, kPlan[ui.view.step].distance, SoundKey(kPlan[ui.view.step].sound));
}

void CancelTake(Ui& ui)
{
    const Command command = Command::Cancel;
    if (g_commands != nullptr) xQueueSend(g_commands, &command, 0);
    ESP_LOGI(kTag, "take %d cancelled", ui.view.step + 1);
    GoToStep(ui, ui.view.step);
}

void FinishTake(Ui& ui)
{
    const int step = ui.view.step;
    xSemaphoreTake(g_resultsLock, portMAX_DELAY);
    TakeRecord& take = g_rtc.takes[step];
    take.state = kMeasured;
    take.levels = g_takeLevels;
    take.overflows = g_takeOverflows;
    MicCheck::Seal(g_rtc);
    xSemaphoreGive(g_resultsLock);
    LogTake(step);
    ui.view.screen = Screen::Result;
    if (g_writer != nullptr && g_card.load() && g_takeBuffer != nullptr)
    {
        g_wavStep.store(step);
        g_wavRun.store(g_rtc.run);
        g_saving.store(true);
    }
    WakeWriter();
}

void Next(Ui& ui);

void Skip(Ui& ui)
{
    const int step = ui.view.step;
    if (g_rtc.takes[step].state != kMeasured)
    {
        xSemaphoreTake(g_resultsLock, portMAX_DELAY);
        g_rtc.takes[step].state = kSkipped;
        MicCheck::Seal(g_rtc);
        xSemaphoreGive(g_resultsLock);
        LogTake(step);
        WakeWriter();
    }
    Next(ui);
}

void OnKey(Ui& ui, uint8_t key)
{
    using namespace Keyboard;
    View& view = ui.view;
    switch (view.screen)
    {
    case Screen::Intro:
        if (key == kKeyOk) NewRun(ui);
        else if (key == kKeyEsc && view.canResume) ShowTable(ui, false);
        break;
    case Screen::Ready:
        if (key == kKeyOk && !ui.startPending) StartTake(ui);
        else if (key == kKeyRight) Skip(ui);
        else if (key == kKeyLeft && view.step > 0) GoToStep(ui, view.step - 1);
        else if (key == kKeyEsc && ui.startPending) ui.startPending = false;
        break;
    case Screen::Recording:
        if (key == kKeyEsc) CancelTake(ui);
        break;
    case Screen::Result:
        if (key == kKeyOk || key == kKeyRight) Next(ui);
        else if (key == kKeyEsc) GoToStep(ui, view.step);
        break;
    case Screen::Table:
        if (key == kKeyUp || key == kKeyDel) view.tableFirst = view.tableFirst > 0 ? view.tableFirst - 1 : 0;
        else if (key == kKeyDown || key == kKeySub)
            view.tableFirst = view.tableFirst < kStepCount - kTableRows ? view.tableFirst + 1 : kStepCount - kTableRows;
        else if (key == kKeyOk)
        {
            view.screen = Screen::Intro;
            view.canResume = true;  // esc goes back to the table
            ui.tablePrinting = false;
        }
        else if (key == kKeyRight && view.canResume) GoToStep(ui, FirstOpenStep(g_rtc));
        break;
    }
}

void Next(Ui& ui)
{
    if (ui.view.step + 1 < kStepCount) GoToStep(ui, ui.view.step + 1);
    else ShowTable(ui, false);
}

void UpdateMeter(Ui& ui, int64_t now)
{
    ui.view.levelDb = static_cast<float>(g_levelCentiDb.load(std::memory_order_relaxed)) / 100.0f;
    const float peak = MagnitudeToDb(g_peakSinceRead.exchange(0, std::memory_order_relaxed));
    if (peak >= ui.peakHold)
    {
        ui.peakHold = peak;
        ui.peakHoldSince = now;
    }
    else if (now - ui.peakHoldSince > kPeakHoldUs)
    {
        const float seconds = static_cast<float>(now - ui.lastFrame) / 1e6f;
        ui.peakHold -= kPeakFallDbPerSecond * seconds;
        if (ui.peakHold < peak) ui.peakHold = peak;
    }
    ui.view.peakHoldDb = ui.peakHold;
}

// The keys that do something on this screen at half, ok full; the rest dark.
void UpdateLeds(const Ui& ui)
{
    using namespace Keyboard;
    LedFrame leds{};
    const uint8_t half = Light::PerceivedToLevel(6);
    const uint8_t full = Light::PerceivedToLevel(15);
    auto set = [&](uint8_t key, uint8_t level) { leds[key - 1] = level; };
    switch (ui.view.screen)
    {
    case Screen::Intro:
        set(kKeyOk, full);
        if (ui.view.canResume) set(kKeyEsc, half);
        break;
    case Screen::Ready:
        set(kKeyOk, full);
        set(kKeyRight, half);
        if (ui.view.step > 0) set(kKeyLeft, half);
        break;
    case Screen::Recording: set(kKeyEsc, half); break;
    case Screen::Result:
        set(kKeyOk, full);
        set(kKeyEsc, half);
        break;
    case Screen::Table:
        set(kKeyOk, half);
        set(kKeyUp, half);
        set(kKeyDown, half);
        if (ui.view.canResume) set(kKeyRight, full);
        break;
    }
    setLEDFrame(leds);
}

const char* ResetReason()
{
    switch (esp_reset_reason())
    {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    default: return "other";
    }
}

}  // namespace

extern "C" void app_main()
{
    const bool halReady = CardenzaHAL::Init();
    const auto& report = CardenzaHAL::LastInitReport();
    const esp_app_desc_t* app = esp_app_get_description();
    ESP_LOGI(kTag, "%s %s ready (HAL system=%d keyboard=%d audio=%d display=%d storage=%d)", app->project_name, app->version,
             report.system, report.keyboard, report.audio, report.display, report.storage);
    if (!report.display || !report.keyboard)
    {
        ESP_LOGE(kTag, "no screen or keys: nothing to do");
        return;
    }
    (void)halReady;  // the codec may have failed: the microphone does not need it

    g_resultsLock = xSemaphoreCreateMutex();
    g_card.store(report.storage && Storage::IsMounted());
    g_takeBuffer = static_cast<int16_t*>(heap_caps_malloc(kTakeSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_takeBuffer == nullptr) ESP_LOGW(kTag, "no PSRAM for a take: no WAVs (the numbers are measured all the same)");
    if (g_card.load()) xTaskCreate(WriterTask, "mic_writer", 4096, nullptr, 2, &g_writer);

    const bool micReady = StartMicrophone();
    std::printf("MICTEST boot reset=%s app=%s version=%s idf=%s mic=%s format=pdm rate=%u bits=16 ch=1 gain=raw "
                "board_mic_gain=%d card=%d psram_take=%d\n",
                ResetReason(), app->project_name, app->version, app->idf_ver, micReady ? "ok" : "failed",
                static_cast<unsigned>(kSampleRate), Settings::Get(Settings::Id::MicGain), g_card.load() ? 1 : 0,
                g_takeBuffer != nullptr ? 1 : 0);

    Keyboard::enableLED(true);
    static Ui ui;
    ui.view.results = &g_rtc;
    ui.haveResults = Intact(g_rtc);
    if (ui.haveResults)
    {
        ESP_LOGI(kTag, "results of run %u kept through the reset", static_cast<unsigned>(g_rtc.run));
        ShowTable(ui, true);
    }
    else
    {
        ui.view.screen = Screen::Intro;
        ui.view.canResume = false;
    }

    bool held[Keyboard::KEY_COUNT + 1] = {};
    int64_t repeatAt = 0;
    TickType_t wake = xTaskGetTickCount();
    ui.lastFrame = esp_timer_get_time();
    for (;;)
    {
        const int64_t now = esp_timer_get_time();
        Keyboard::updateKeyList();
        bool down[Keyboard::KEY_COUNT + 1] = {};
        for (const auto& point : Keyboard::keyList())
        {
            const uint8_t key = Keyboard::getKeyNum(point);
            if (key >= 1 && key <= Keyboard::KEY_COUNT) down[key] = true;
        }
        for (uint8_t key = 1; key <= Keyboard::KEY_COUNT; ++key)
        {
            if (down[key] && !held[key])
            {
                OnKey(ui, key);
                repeatAt = now + 400 * 1000;
            }
            // The table scrolls on while an arrow is held.
            else if (down[key] && held[key] && ui.view.screen == Screen::Table && now >= repeatAt &&
                     (key == Keyboard::kKeyUp || key == Keyboard::kKeyDown || key == Keyboard::kKeyDel || key == kKeySub))
            {
                OnKey(ui, key);
                repeatAt = now + 100 * 1000;
            }
            held[key] = down[key];
        }

        if (ui.view.screen == Screen::Ready && ui.startPending && !g_saving.load()) StartTake(ui);
        if (ui.view.screen == Screen::Recording)
        {
            ui.view.elapsed = static_cast<float>(g_takeSamples.load(std::memory_order_relaxed)) / kSampleRate;
            if (g_takeState.load(std::memory_order_acquire) == 2) FinishTake(ui);
        }
        if (ui.tablePrinting && now - ui.lastTablePrint >= kTablePrintUs)
        {
            ui.lastTablePrint = now;
            PrintTable();
        }

        UpdateMeter(ui, now);
        ui.lastFrame = now;
        ui.view.waiting = ui.startPending;
        ui.view.card = CardNow(ui);
        ui.view.run = static_cast<int>(g_rtc.run);
        if (!ui.haveResults && ui.view.screen == Screen::Table) ui.view.screen = Screen::Intro;
        UpdateLeds(ui);
        Draw(g_frame, ui.view);
        Display::PresentChanged(g_frame);
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(20));
    }
}
