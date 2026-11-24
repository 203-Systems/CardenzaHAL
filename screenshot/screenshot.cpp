// Screenshots (screenshot/screenshot.h): the frame, the PNG on the card, the upload queue, the uploads (on the
// presence service's task) and USB's SCREENSHOT. The keys and the effect on the screen are the side key's task's
// (system/menu.cpp); the decisions are screenshot/logic.cpp's.
#include "screenshot/screenshot.h"

#include <sdkconfig.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "display/display.h"
#include "display/internal/snapshot.h"
#include "keyboard/internal/space_filter.h"
#include "net/internal/presence_side.h"
#include "net/presence_logic.h"
#include "screenshot/internal.h"
#include "screenshot/png.h"
#include "storage/storage.h"
#include "system/internal/hooks.h"
#include "system/internal/key_task.h"
#include "system/settings.h"

namespace CardenzaHAL::Screenshot {

namespace {

constexpr char kTag[] = "CardenzaShot";
constexpr int kWidth = Display::WIDTH, kHeight = Display::HEIGHT;
constexpr std::size_t kFrameBytes = static_cast<std::size_t>(kWidth) * kHeight * sizeof(uint16_t);
constexpr std::size_t kThumbPixels = static_cast<std::size_t>(Logic::Effect::kThumb.w) * Logic::Effect::kThumb.h;
// The screenshots' task: encodes and writes, then waits for the next shot a while before it goes.
constexpr uint32_t kStackBytes = 6144;
constexpr UBaseType_t kPriority = 1;  // above idle only: the firmware comes first
constexpr uint32_t kIdleMs = 10000;
// Over USB and from Take: the firmware's next frame is waited for this long (the development channel's SCREEN's).
constexpr uint32_t kSnapshotWaitMs = 250;
// The community takes PNGs of at most 256 KiB (a 240 x 135 one is 110 KB at worst).
constexpr std::size_t kUploadMost = 262144;
// The queue is read whole (64 KB: about 1500 shots waiting); longer, it is rewritten with what is pending.
constexpr std::size_t kQueueReadMost = 65536;
constexpr std::size_t kQueueCompactAt = 16384;
constexpr int kUploadsPerAnswer = 4;
// Internal RAM the encoder leaves to the firmware when it has to work there (no PSRAM).
constexpr std::size_t kInternalFloor = 48 * 1024;

std::atomic<bool> g_started{false};
std::atomic<bool> g_busy{false};
std::atomic<bool> g_showing{false};
std::atomic<bool> g_spaceWait{false};
std::atomic<bool> g_claimed{false};

// The gesture's state for the keyboard (detail::PublishWindow), and what the firmware sees of space.
portMUX_TYPE g_windowMux = portMUX_INITIALIZER_UNLOCKED;
Logic::HomeGesture::Window g_window;
Logic::SpaceGate g_gate;  // the firmware's task's (updateKeyList)

// The effect's mailbox (detail::TakeEffect, EffectRevision).
portMUX_TYPE g_effectMux = portMUX_INITIALIZER_UNLOCKED;
detail::EffectPost g_post;
bool g_postFresh = false;
uint32_t g_revision = 0;
Card::Words g_words = Card::Words::Taken;
char g_name[84] = "";

std::mutex g_listenerMutex;
Listener g_listener = nullptr;
void* g_listenerContext = nullptr;

// The screenshots' task and what it is given.
struct Job {
    uint16_t* frame = nullptr;           // a copy, the job's to free (or nullptr)
    const uint16_t* pixels = nullptr;    // what to encode (`frame`, or the firmware's own buffer)
    bool grab = false;                   // the frame is to be taken first (Take)
    Options options;
};
std::mutex g_workerMutex;
QueueHandle_t g_jobs = nullptr;
bool g_workerRunning = false;

// The folder and the queue: one writer at a time (the screenshots' task, USB's worker, the uploads).
std::mutex g_cardMutex;
bool g_numbersKnown = false;
uint32_t g_lastNumber = 0;
char g_tag[Logic::kTagMax + 1] = "Firmware";
// Uploads: something may be waiting (unknown at the start), and nothing more this run (the community said so).
std::atomic<bool> g_uploadMaybe{true};
std::atomic<bool> g_uploadStopped{false};

uint32_t NowMs()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

void* SlowAlloc(std::size_t bytes)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// PSRAM, else internal RAM while the firmware keeps kInternalFloor.
void* WorkAlloc(std::size_t bytes)
{
    if (void* block = SlowAlloc(bytes)) return block;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < bytes + kInternalFloor) return nullptr;
    return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

Card::Words WordsFor(Status status)
{
    switch (status)
    {
        case Status::Saved: return Card::Words::Saved;
        case Status::NoCard: return Card::Words::NoCard;
        case Status::WriteFailed: return Card::Words::Failed;
        case Status::NoMemory: return Card::Words::NoMemory;
        case Status::NoFrame: return Card::Words::Unavailable;
        case Status::Taken:
        case Status::Busy: return Card::Words::Taken;
    }
    return Card::Words::Taken;
}

void PostEffect(uint16_t* thumb, Card::Words words)
{
    uint16_t* old = nullptr;
    portENTER_CRITICAL(&g_effectMux);
    if (g_postFresh) old = g_post.thumb;
    g_post = detail::EffectPost{};
    g_post.thumb = thumb;
    g_post.words = words;
    g_postFresh = true;
    g_words = words;
    g_name[0] = 0;
    ++g_revision;
    portEXIT_CRITICAL(&g_effectMux);
    heap_caps_free(old);
}

void UpdateEffect(Card::Words words, const char* name)
{
    portENTER_CRITICAL(&g_effectMux);
    g_words = words;
    std::snprintf(g_name, sizeof(g_name), "%s", name != nullptr ? name : "");
    ++g_revision;
    portEXIT_CRITICAL(&g_effectMux);
}

// The shot at a quarter of its size, for the card (PSRAM when there is some); nullptr without memory.
uint16_t* MakeThumb(const uint16_t* pixels)
{
    auto* thumb = static_cast<uint16_t*>(heap_caps_malloc_prefer(kThumbPixels * sizeof(uint16_t), 2,
                                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (thumb == nullptr) return nullptr;
    for (int ty = 0; ty < Logic::Effect::kThumb.h; ++ty)
        for (int tx = 0; tx < Logic::Effect::kThumb.w; ++tx)
            thumb[ty * Logic::Effect::kThumb.w + tx] =
                pixels[Logic::Effect::ThumbSourceY(ty) * kWidth + Logic::Effect::ThumbSourceX(tx)];
    return thumb;
}

void Finish(const Shot& shot)
{
    Listener listener;
    void* context;
    {
        std::lock_guard<std::mutex> lock(g_listenerMutex);
        listener = g_listener;
        context = g_listenerContext;
    }
    g_busy.store(false);
    if (listener != nullptr) listener(shot, context);
}

// The firmware's frame now: the next one it presents (up to `waitMs`), else its last framebuffer, else what it
// drew through the panel's handles. `owned`: a copy in PSRAM (to free), or nullptr when the result is the
// firmware's own buffer (no PSRAM to spare: read where it is). nullptr: nothing to capture.
const uint16_t* Grab(uint32_t waitMs, uint16_t*& owned)
{
    owned = static_cast<uint16_t*>(SlowAlloc(kFrameBytes));
    if (owned != nullptr && waitMs != 0)
    {
        Display::detail::RequestSnapshot(owned);
        for (uint32_t waited = 0; waited < waitMs; waited += 10)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            if (Display::detail::SnapshotTaken()) return owned;
        }
        Display::detail::RequestSnapshot(nullptr);
        if (Display::detail::SnapshotTaken()) return owned;
    }
    const uint16_t* last = Display::detail::LastFramebuffer();
    if (last == nullptr) last = Display::detail::PanelDrawsCopy();
    if (last == nullptr)
    {
        heap_caps_free(owned);
        owned = nullptr;
        return nullptr;
    }
    if (owned == nullptr) return last;
    std::memcpy(owned, last, kFrameBytes);
    return owned;
}

// ---- On the card --------------------------------------------------------------------------------------------

void FolderPath(const char* name, char* out, std::size_t capacity)
{
    std::snprintf(out, capacity, "%s/%s", kFolder, name);
}

// The highest shot-NNNN in the folder, and a temporary file a power cut left behind goes.
void ScanFolder()
{
    DIR* dir = opendir(kFolder);
    if (dir == nullptr) return;
    while (const dirent* entry = readdir(dir))
    {
        g_lastNumber = std::max(g_lastNumber, Logic::ShotNumber(entry->d_name));
        const std::size_t length = std::strlen(entry->d_name);
        if (entry->d_name[0] == '.' && length > 5 && std::strcmp(entry->d_name + length - 4, ".tmp") == 0)
        {
            char path[160];
            FolderPath(entry->d_name, path, sizeof(path));
            (void)unlink(path);
        }
    }
    closedir(dir);
}

bool Exists(const char* name)
{
    char path[160];
    FolderPath(name, path, sizeof(path));
    struct stat info;
    return stat(path, &info) == 0;
}

// A name no file has yet (g_cardMutex held): dated when the clock is set, else numbered.
bool ChooseName(char* name, std::size_t capacity)
{
    if (mkdir(kFolder, 0775) != 0 && errno != EEXIST) return false;
    const time_t now = std::time(nullptr);
    if (Logic::ClockSet(static_cast<int64_t>(now)))
    {
        for (int sequence = 1; sequence < 100; ++sequence)
        {
            if (!Logic::DatedName(static_cast<int64_t>(now), sequence, g_tag, name, capacity)) return false;
            if (!Exists(name)) return true;
        }
        return false;
    }
    if (!g_numbersKnown)
    {
        ScanFolder();
        g_numbersKnown = true;
    }
    for (int tries = 0; tries < 100; ++tries)
    {
        if (!Logic::NumberedName(++g_lastNumber, g_tag, name, capacity)) return false;
        if (!Exists(name)) return true;
    }
    return false;
}

// One line of the upload queue (g_cardMutex held).
bool AppendQueue(char kind, const char* name)
{
    char line[Logic::kNameMax + 8];
    const std::size_t length = Logic::Queue::Line(kind, name, line, sizeof(line));
    if (length == 0) return false;
    char path[160];
    FolderPath(Logic::Queue::kFileName, path, sizeof(path));
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0664);
    if (fd < 0) return false;
    const bool ok = write(fd, line, length) == static_cast<ssize_t>(length) && fsync(fd) == 0;
    return close(fd) == 0 && ok;
}

// Where the encoder's bytes go: the file, memory (USB), or both.
struct Output {
    int fd = -1;
    bool fileOk = true;
    uint8_t* memory = nullptr;
    std::size_t used = 0, capacity = 0;
    bool memoryOk = true;

    static bool Write(void* context, const uint8_t* data, std::size_t size)
    {
        auto& out = *static_cast<Output*>(context);
        if (out.fd >= 0 && out.fileOk)
            for (std::size_t done = 0; done < size;)
            {
                const ssize_t wrote = write(out.fd, data + done, size - done);
                if (wrote <= 0)
                {
                    out.fileOk = false;
                    break;
                }
                done += static_cast<std::size_t>(wrote);
            }
        if (out.memory != nullptr && out.memoryOk)
        {
            if (out.used + size > out.capacity)
            {
                const std::size_t grown = std::min(std::max(out.capacity * 2, out.used + size), kUploadMost);
                void* bigger = grown >= out.used + size
                                   ? heap_caps_realloc(out.memory, grown, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                   : nullptr;
                if (bigger == nullptr) out.memoryOk = false;
                else
                {
                    out.memory = static_cast<uint8_t*>(bigger);
                    out.capacity = grown;
                }
            }
            if (out.memoryOk)
            {
                std::memcpy(out.memory + out.used, data, size);
                out.used += size;
            }
        }
        return (out.fd >= 0 && out.fileOk) || (out.memory != nullptr && out.memoryOk);
    }
};

// Encodes `pixels` and writes them to the card (`save`) and/or into memory (`memory`: the PNG and its size, the
// caller's to free). The file goes to a temporary name, is synced and renamed: a power cut leaves the old state
// or the whole file. With screenshot_upload on, a saved shot is queued for the community.
Status Encode(const uint16_t* pixels, bool save, uint8_t** memory, std::size_t* memoryBytes, Shot& shot)
{
    shot.name[0] = 0;
    shot.bytes = 0;
    if (memory != nullptr) *memory = nullptr;
    if (!save && memory == nullptr) return Status::Taken;
    const std::size_t workBytes = Png::WorkBytes(kWidth);
    void* work = WorkAlloc(workBytes);
    if (work == nullptr) return Status::NoMemory;
    Output out;
    Status status = Status::Taken;
    char temp[Logic::kNameMax + 8] = "";
    std::unique_lock<std::mutex> card(g_cardMutex, std::defer_lock);
    if (save)
    {
        if (!Storage::IsMounted())
            status = Status::NoCard;
        else
        {
            card.lock();
            char tempPath[160];
            if (!ChooseName(shot.name, sizeof(shot.name)) || !Logic::TempName(shot.name, temp, sizeof(temp)))
                status = Status::WriteFailed;
            else
            {
                FolderPath(temp, tempPath, sizeof(tempPath));
                out.fd = open(tempPath, O_WRONLY | O_CREAT | O_TRUNC, 0664);
                if (out.fd < 0) status = Status::WriteFailed;
            }
            if (status == Status::WriteFailed)
            {
                shot.name[0] = 0;
                card.unlock();
                (void)Storage::CheckAfterIoError();
            }
        }
    }
    if (memory != nullptr)
    {
        out.capacity = 64 * 1024;
        out.memory = static_cast<uint8_t*>(SlowAlloc(out.capacity));
        if (out.memory == nullptr) out.memoryOk = false;
    }
    if (out.fd < 0 && out.memory == nullptr)
    {
        heap_caps_free(work);
        return status == Status::Taken ? Status::NoMemory : status;
    }
    const Png::Result result = Png::Encode(pixels, kWidth, kHeight, kWidth, work, workBytes, Output::Write, &out);
    heap_caps_free(work);
    if (out.fd >= 0)
    {
        char tempPath[160], finalPath[160];
        FolderPath(temp, tempPath, sizeof(tempPath));
        FolderPath(shot.name, finalPath, sizeof(finalPath));
        bool ok = result.ok && out.fileOk && fsync(out.fd) == 0;
        ok = close(out.fd) == 0 && ok;
        if (ok) ok = rename(tempPath, finalPath) == 0;
        if (!ok)
        {
            (void)unlink(tempPath);
            shot.name[0] = 0;
            status = Status::WriteFailed;
            card.unlock();
            (void)Storage::CheckAfterIoError();
        }
        else
        {
            status = Status::Saved;
            shot.bytes = static_cast<uint32_t>(result.bytes);
            if (Settings::Get(Settings::Id::ScreenshotUpload) != 0)
            {
                if (AppendQueue(Logic::Queue::kQueued, shot.name))
                    g_uploadMaybe.store(true);
                else
                    ESP_LOGW(kTag, "%s: not queued for the community (the card refused the queue)", shot.name);
            }
            card.unlock();
        }
    }
    if (memory != nullptr)
    {
        if (result.ok && out.memory != nullptr && out.memoryOk)
        {
            *memory = out.memory;
            if (memoryBytes != nullptr) *memoryBytes = out.used;
            if (shot.bytes == 0) shot.bytes = static_cast<uint32_t>(out.used);
        }
        else
            heap_caps_free(out.memory);
    }
    return status;
}

void LogShot(const Shot& shot, uint32_t startMs)
{
    if (shot.status == Status::Saved)
        ESP_LOGI(kTag, "saved %s/%s (%u bytes, %u ms)", kFolder, shot.name, static_cast<unsigned>(shot.bytes),
                 static_cast<unsigned>(NowMs() - startMs));
    else if (shot.status != Status::Taken)
        ESP_LOGW(kTag, "screenshot: %s", StatusName(shot.status));
}

// ---- The screenshots' task ------------------------------------------------------------------------------------

void RunJob(Job& job)
{
    const uint32_t start = NowMs();
    Shot shot;
    if (job.grab)
    {
        job.pixels = Grab(kSnapshotWaitMs, job.frame);
        if (job.pixels == nullptr)
        {
            shot.status = Status::NoFrame;
            if (job.options.effect) PostEffect(nullptr, Card::Words::Unavailable);
            LogShot(shot, start);
            Finish(shot);
            return;
        }
        if (job.options.effect)
            PostEffect(MakeThumb(job.pixels), !job.options.save        ? Card::Words::Taken
                                              : Storage::IsMounted() ? Card::Words::Saved
                                                                     : Card::Words::NoCard);
    }
    shot.status = Encode(job.pixels, job.options.save, nullptr, nullptr, shot);
    if (job.options.effect) UpdateEffect(WordsFor(shot.status), shot.name);
    heap_caps_free(job.frame);
    LogShot(shot, start);
    Finish(shot);
}

void Worker(void*)
{
    for (;;)
    {
        Job job;
        if (xQueueReceive(g_jobs, &job, pdMS_TO_TICKS(kIdleMs)) == pdTRUE)
        {
            RunJob(job);
            continue;
        }
        std::lock_guard<std::mutex> lock(g_workerMutex);
        if (uxQueueMessagesWaiting(g_jobs) != 0) continue;
        g_workerRunning = false;
        break;
    }
    vTaskDelete(nullptr);
}

bool Submit(const Job& job)
{
    std::lock_guard<std::mutex> lock(g_workerMutex);
    if (g_jobs == nullptr) g_jobs = xQueueCreate(2, sizeof(Job));
    if (g_jobs == nullptr || xQueueSend(g_jobs, &job, 0) != pdTRUE) return false;
    if (g_workerRunning) return true;
    g_workerRunning = true;
    if (xTaskCreate(Worker, "sys_shot", kStackBytes, nullptr, kPriority, nullptr) == pdPASS) return true;
    g_workerRunning = false;
    Job dropped;
    (void)xQueueReceive(g_jobs, &dropped, 0);
    ESP_LOGE(kTag, "no memory for the screenshots' task");
    return false;
}

// ---- What the firmware sees of space ------------------------------------------------------------------------

bool FilterSpace(bool down, uint32_t nowMs)
{
    portENTER_CRITICAL(&g_windowMux);
    const Logic::HomeGesture::Window window = g_window;
    portEXIT_CRITICAL(&g_windowMux);
    const bool systems = Logic::HomeGesture::SpaceIsSystems(window, System::detail::HomeButtonRaw(), nowMs);
    const bool shown = g_gate.Update(down, systems, g_spaceWait.load(std::memory_order_relaxed), nowMs);
    if (g_gate.TakeClaimed()) g_claimed.store(true);
    return shown;
}

// ---- The uploads (on the presence service's task: net/internal/presence_side.h) -----------------------------

// The queue's text (g_cardMutex held), at most kQueueReadMost bytes (PSRAM when there is some). nullptr: none.
char* ReadQueue(std::size_t& length)
{
    length = 0;
    char path[160];
    FolderPath(Logic::Queue::kFileName, path, sizeof(path));
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return nullptr;
    struct stat info;
    const std::size_t size = fstat(fd, &info) == 0 ? std::min<std::size_t>(info.st_size, kQueueReadMost) : 0;
    char* text = size != 0 ? static_cast<char*>(WorkAlloc(size + 1)) : nullptr;
    if (text != nullptr)
    {
        while (length < size)
        {
            const ssize_t got = read(fd, text + length, size - length);
            if (got <= 0) break;
            length += static_cast<std::size_t>(got);
        }
        text[length] = 0;
    }
    close(fd);
    return text;
}

// The queue with only what is pending, or gone when nothing is (g_cardMutex held).
void CompactQueue(const char* text, std::size_t length, std::size_t pending)
{
    char path[160];
    FolderPath(Logic::Queue::kFileName, path, sizeof(path));
    if (pending == 0)
    {
        (void)unlink(path);
        return;
    }
    char* kept = static_cast<char*>(WorkAlloc(length + 1));
    if (kept == nullptr) return;
    const std::size_t keptLength = Logic::Queue::Compact(text, length, kept, length + 1);
    char temp[160];
    FolderPath(".upload-queue.tmp", temp, sizeof(temp));
    const int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    bool ok = fd >= 0 && write(fd, kept, keptLength) == static_cast<ssize_t>(keptLength) && fsync(fd) == 0;
    if (fd >= 0) ok = close(fd) == 0 && ok;
    // FAT renames onto nothing: the old queue goes first (a cut here leaves the temporary one, read next time).
    if (ok) ok = unlink(path) == 0 && rename(temp, path) == 0;
    if (!ok) (void)unlink(temp);
    heap_caps_free(kept);
}

// The oldest pending shot (g_cardMutex held); false when none is.
bool NextPending(char* name, std::size_t capacity)
{
    std::size_t length = 0;
    char* text = ReadQueue(length);
    if (text == nullptr)
    {
        // A compaction a power cut interrupted left only the temporary queue.
        char path[160], temp[160];
        FolderPath(Logic::Queue::kFileName, path, sizeof(path));
        FolderPath(".upload-queue.tmp", temp, sizeof(temp));
        struct stat info;
        if (stat(path, &info) != 0 && stat(temp, &info) == 0 && rename(temp, path) == 0) text = ReadQueue(length);
        if (text == nullptr) return false;
    }
    std::size_t pending = 0;
    const bool found = Logic::Queue::FirstPending(text, length, name, capacity, &pending);
    if (pending == 0 || length >= kQueueCompactAt) CompactQueue(text, length, pending);
    heap_caps_free(text);
    return found;
}

void UploadSide(const Presence::detail::SideRequests& requests)
{
    if (!g_uploadMaybe.load() || g_uploadStopped.load()) return;
    if (Settings::Get(Settings::Id::ScreenshotUpload) == 0 || !Storage::IsMounted()) return;
    for (int i = 0; i < kUploadsPerAnswer && !requests.stop(requests.context); ++i)
    {
        char name[Logic::kNameMax + 1];
        int fd = -1;
        uint64_t bytes = 0;
        time_t taken = 0;
        {
            std::lock_guard<std::mutex> lock(g_cardMutex);
            if (!NextPending(name, sizeof(name)))
            {
                g_uploadMaybe.store(false);
                return;
            }
            char path[160];
            FolderPath(name, path, sizeof(path));
            struct stat info;
            if (stat(path, &info) != 0 || info.st_size <= 0 || static_cast<std::size_t>(info.st_size) > kUploadMost ||
                (fd = open(path, O_RDONLY)) < 0)
            {
                ESP_LOGW(kTag, "%s: not uploaded (the file is gone or too large)", name);
                (void)AppendQueue(Logic::Queue::kSkipped, name);
                continue;
            }
            bytes = static_cast<uint64_t>(info.st_size);
            taken = info.st_mtime;
        }
        char takenText[24] = "";
        if (Logic::ClockSet(static_cast<int64_t>(taken)))
            std::snprintf(takenText, sizeof(takenText), "%lld", static_cast<long long>(taken));
        Net::Header headers[3];
        std::size_t count = 0;
        headers[count++] = {"Content-Type", "image/png"};
        headers[count++] = {"X-Screenshot-Name", name};
        if (takenText[0] != 0) headers[count++] = {"X-Taken-At", takenText};
        char answer[512];
        answer[0] = 0;
        const int status = requests.post(requests.context, "/device/screenshots", headers, count, fd, bytes, answer,
                                         sizeof(answer));
        close(fd);
        char code[32];
        Presence::Logic::ErrorCode(answer, std::strlen(answer), code, sizeof(code));
        const Logic::Upload verdict = Logic::Classify(status, code);
        std::lock_guard<std::mutex> lock(g_cardMutex);
        switch (verdict)
        {
            case Logic::Upload::Done:
                ESP_LOGI(kTag, "%s uploaded to the community", name);
                (void)AppendQueue(Logic::Queue::kUploaded, name);
                break;
            case Logic::Upload::Skip:
                ESP_LOGW(kTag, "%s: the community refused it (%d %s): skipped", name, status, code);
                (void)AppendQueue(Logic::Queue::kSkipped, name);
                break;
            case Logic::Upload::StopRun:
                ESP_LOGW(kTag, "uploads stop until the next start: the community answered %d %s", status, code);
                g_uploadStopped.store(true);
                return;
            case Logic::Upload::Later:
            case Logic::Upload::SignedOut:
                return;  // the next answer of the inbox, or the next start
        }
    }
}

}  // namespace

// ---- The public calls ----------------------------------------------------------------------------------------

const char* StatusName(Status status)
{
    switch (status)
    {
        case Status::Saved: return "saved";
        case Status::Taken: return "taken";
        case Status::NoCard: return "no SD card";
        case Status::WriteFailed: return "the card refused the write";
        case Status::NoFrame: return "nothing to capture (this firmware draws in a way the HAL cannot see)";
        case Status::NoMemory: return "no memory";
        case Status::Busy: return "busy";
    }
    return "?";
}

void Start()
{
#if CONFIG_CARDENZA_HAL_SCREENSHOT
    if (g_started.exchange(true)) return;
    const esp_app_desc_t* app = esp_app_get_description();
    Logic::FirmwareTag(app != nullptr ? app->project_name : nullptr, g_tag, sizeof(g_tag));
    Keyboard::detail::SetSpaceFilter(FilterSpace);
    Presence::detail::SetSideWork(UploadSide);
    System::detail::StartKeyTask();
#endif
}

bool Take(const Options& options)
{
    if (!g_started.load() || g_busy.exchange(true)) return false;
    Job job;
    job.grab = true;
    job.options = options;
    if (Submit(job)) return true;
    g_busy.store(false);
    return false;
}

void SetListener(Listener listener, void* context)
{
    std::lock_guard<std::mutex> lock(g_listenerMutex);
    g_listener = listener;
    g_listenerContext = context;
}

bool IsShowing()
{
    return g_showing.load();
}

void SetSpaceWait(bool wait)
{
    g_spaceWait.store(wait);
}

bool SpaceWait()
{
    return g_spaceWait.load();
}

// ---- Inside the HAL ---------------------------------------------------------------------------------------------

namespace detail {

void PublishWindow(const Logic::HomeGesture::Window& window)
{
    portENTER_CRITICAL(&g_windowMux);
    g_window = window;
    portEXIT_CRITICAL(&g_windowMux);
}

bool TakeClaimedSpace()
{
    return g_claimed.exchange(false);
}

void FromKeys(const uint16_t* frame)
{
    if (!g_started.load()) return;
    if (g_busy.exchange(true))
    {
        ESP_LOGW(kTag, "HOME + space: a screenshot is still being taken");
        return;
    }
    const uint32_t start = NowMs();
    const uint16_t* source = frame != nullptr ? frame : Display::detail::LastFramebuffer();
    if (source == nullptr) source = Display::detail::PanelDrawsCopy();
    if (source == nullptr)
    {
        PostEffect(nullptr, Card::Words::Unavailable);
        Shot shot;
        shot.status = Status::NoFrame;
        LogShot(shot, start);
        Finish(shot);
        return;
    }
    Job job;
    job.frame = static_cast<uint16_t*>(SlowAlloc(kFrameBytes));
    if (job.frame != nullptr) std::memcpy(job.frame, source, kFrameBytes);
    else if (source == frame)
    {
        // No room for a copy: the menu's capture goes when HOME is let go, so the firmware's own buffer is read
        // where it is instead (a frame it may be drawing into: the price of no memory).
        source = Display::detail::LastFramebuffer();
        if (source == nullptr) source = Display::detail::PanelDrawsCopy();
        if (source == nullptr)
        {
            PostEffect(nullptr, Card::Words::NoMemory);
            Shot shot;
            shot.status = Status::NoMemory;
            LogShot(shot, start);
            Finish(shot);
            return;
        }
    }
    job.pixels = job.frame != nullptr ? job.frame : source;
    PostEffect(MakeThumb(job.pixels), Storage::IsMounted() ? Card::Words::Saved : Card::Words::NoCard);
    if (Submit(job)) return;
    heap_caps_free(job.frame);
    UpdateEffect(Card::Words::NoMemory, "");
    Shot shot;
    shot.status = Status::NoMemory;
    LogShot(shot, start);
    Finish(shot);
}

bool TakeEffect(EffectPost& post)
{
    portENTER_CRITICAL(&g_effectMux);
    const bool fresh = g_postFresh;
    if (fresh)
    {
        post = g_post;
        g_post.thumb = nullptr;
        g_postFresh = false;
    }
    portEXIT_CRITICAL(&g_effectMux);
    return fresh;
}

uint32_t EffectRevision(EffectPost& post)
{
    portENTER_CRITICAL(&g_effectMux);
    post.words = g_words;
    std::memcpy(post.name, g_name, sizeof(post.name));
    const uint32_t revision = g_revision;
    portEXIT_CRITICAL(&g_effectMux);
    return revision;
}

void SetShowing(bool showing)
{
    g_showing.store(showing);
}

const char* TakeForUsb(uint8_t flags, const UsbOut& out)
{
    if (!g_started.load()) return "off";
    if (g_busy.exchange(true)) return "busy";
    const uint32_t start = NowMs();
    uint16_t* owned = nullptr;
    const uint16_t* pixels = Grab(kSnapshotWaitMs, owned);
    if (pixels == nullptr)
    {
        g_busy.store(false);
        return "no framebuffer";  // 203cli reads the exact words (docs/usb-dev.md)
    }
    const bool json = flags & Logic::Usb::kJson;
    const bool save = flags & Logic::Usb::kSave, give = !json && (flags & Logic::Usb::kReturn);
    const bool raw = flags & Logic::Usb::kRaw, effect = !(flags & Logic::Usb::kQuiet);
    if (effect)
        PostEffect(MakeThumb(pixels), !save                  ? Card::Words::Taken
                                      : Storage::IsMounted() ? Card::Words::Saved
                                                             : Card::Words::NoCard);
    uint8_t* png = nullptr;
    std::size_t pngBytes = 0;
    Shot shot;
    shot.status = Encode(pixels, save, give && !raw ? &png : nullptr, &pngBytes, shot);
    if (effect) UpdateEffect(WordsFor(shot.status), shot.name);
    using Logic::Usb::Saved;
    const Saved saved = !save                             ? Saved::NotAsked
                        : shot.status == Status::Saved   ? Saved::Saved
                        : shot.status == Status::NoCard  ? Saved::NoCard
                                                          : Saved::Failed;
    uint8_t header[16 + Logic::kNameMax];
    const auto send = [&](Logic::Usb::Format format, uint32_t bytes) {
        const std::size_t length = Logic::Usb::Header(format, saved, kWidth, kHeight, shot.name, bytes, header, sizeof(header));
        return length != 0 && out.write(out.context, header, length);
    };
    if (json)
    {
        static constexpr const char* kSaved[] = {"not_asked", "saved", "no_card", "card_refused"};
        char text[64 + 2 * Logic::kNameMax];
        const int length = std::snprintf(
            text, sizeof(text), "{\"name\":\"%s\",\"path\":\"%s%s\",\"saved\":\"%s\",\"bytes\":%u,\"width\":%d,\"height\":%d}",
            shot.name, shot.name[0] != 0 ? "screenshot/" : "", shot.name, kSaved[static_cast<int>(saved)],
            static_cast<unsigned>(shot.bytes), kWidth, kHeight);
        if (length > 0) (void)out.write(out.context, text, std::min<std::size_t>(length, sizeof(text) - 1));
    }
    else if (!give)
        (void)send(Logic::Usb::Format::Png, 0);
    else if (png != nullptr)
    {
        if (send(Logic::Usb::Format::Png, static_cast<uint32_t>(pngBytes))) (void)out.write(out.context, png, pngBytes);
    }
    else if (!raw && saved == Saved::Saved)
    {
        // No memory for the PNG beside the file: it is read back from the card.
        char path[160];
        FolderPath(shot.name, path, sizeof(path));
        const int fd = open(path, O_RDONLY);
        struct stat info;
        if (fd >= 0 && fstat(fd, &info) == 0 && send(Logic::Usb::Format::Png, static_cast<uint32_t>(info.st_size)))
        {
            char block[512];
            ssize_t got;
            while ((got = read(fd, block, sizeof(block))) > 0 && out.write(out.context, block, static_cast<std::size_t>(got))) {}
        }
        if (fd >= 0) close(fd);
    }
    else if (send(Logic::Usb::Format::Raw, static_cast<uint32_t>(kFrameBytes)))
        (void)out.write(out.context, pixels, kFrameBytes);  // u16 little-endian, as the processor holds them
    heap_caps_free(png);
    heap_caps_free(owned);
    LogShot(shot, start);
    Finish(shot);
    return nullptr;
}

}  // namespace detail

}  // namespace CardenzaHAL::Screenshot
