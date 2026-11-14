// The community presence service (net/presence.h): a low-priority task that keeps the device reachable
// from the 203 community while any firmware runs, and hands commands over to the Launcher. Its
// decisions are in net/presence_logic.cpp (tested on the host); this file is the task around them.
#include "net/presence.h"

#include <sdkconfig.h>

#if CONFIG_CARDENZA_HAL_PRESENCE

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "audio/audio.h"
#include "net/internal/shared_use.h"
#include "net/presence_logic.h"
#include "system/launcher.h"

namespace CardenzaHAL::Presence {

namespace {

using Net::Internal::User;

constexpr char kTag[] = "CardenzaPresence";
// TLS's handshake runs on this stack (6-8 KB, Net's numbers); the high-water mark is logged once.
constexpr uint32_t kStackBytes = 8192;
constexpr UBaseType_t kPriority = 1;  // above idle only: the firmware's tasks always come first
constexpr BaseType_t kCore = 0;       // beside Wi-Fi's and lwIP's tasks
constexpr std::size_t kAnswerBytes = 16 * 1024;
constexpr uint32_t kRequestTimeoutMs = 10000;
constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr uint32_t kGiveWayMs = 5000;
constexpr uint32_t kStartDelayMs = CONFIG_CARDENZA_HAL_PRESENCE_START_DELAY * 1000u;
#if CONFIG_CARDENZA_HAL_PRESENCE_LOCAL_HTTP
constexpr bool kLocalHttp = true;
#else
constexpr bool kLocalHttp = false;
#endif
#if CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC || CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC
constexpr bool kTlsCanUsePsram = true;
#else
constexpr bool kTlsCanUsePsram = false;
#endif

std::mutex g_lifecycle;  // Start, Enable and the task's end
std::atomic<bool> g_started{false};
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_running{false};
std::atomic<bool> g_signedOut{false};  // until the next start of the device
std::atomic<uint8_t> g_state{static_cast<uint8_t>(State::Off)};
TaskHandle_t g_task = nullptr;  // under g_lifecycle
bool g_delayed = false;         // the start delay is done (the task's own)

std::mutex g_hookMutex;
HandoverHook g_hook = nullptr;
void* g_hookContext = nullptr;

const char* Name(State state)
{
    switch (state)
    {
    case State::Off: return "off";
    case State::Idle: return "idle";
    case State::Connecting: return "connecting";
    case State::Online: return "online";
    case State::Waiting: return "waiting";
    case State::SignedOut: return "signed out";
    case State::HandingOver: return "handing over";
    }
    return "?";
}

void SetState(State state)
{
    const auto previous = static_cast<State>(g_state.exchange(static_cast<uint8_t>(state)));
    if (previous != state) ESP_LOGI(kTag, "%s", Name(state));
}

void Wipe(void* data, std::size_t size)
{
    volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) bytes[i] = 0;
}

bool Stopping()
{
    return !g_enabled.load();
}

bool Cancelled(void*)
{
    return Stopping();
}

// Waits `ms`; Enable(false) wakes it. False when switched off.
bool Wait(uint32_t ms)
{
    const int64_t endUs = esp_timer_get_time() + int64_t{ms} * 1000;
    while (!Stopping())
    {
        const int64_t leftUs = endUs - esp_timer_get_time();
        if (leftUs <= 0) return true;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(leftUs / 1000) + 1);
    }
    return false;
}

void* SlowAlloc(std::size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// Whether there is a reason not to use this binding; logged once per reason.
bool UsableBinding(const Net::LauncherAccount& account, bool receiveFirmware)
{
    if (!account.Bound())
    {
        ESP_LOGI(kTag, "the Launcher has not bound this device to the community");
        return false;
    }
    if (!receiveFirmware)
    {
        ESP_LOGI(kTag, "Receive firmware is off in the Launcher");
        return false;
    }
    if (!Logic::AllowedApi(account.api, kLocalHttp))
    {
        // The token goes only to the community's own hosts, over https.
        ESP_LOGW(kTag, "the binding names a community the device token is not sent to");
        return false;
    }
    return true;
}

enum class Joined : uint8_t { Yes, Idle, Retry, Stopped };

// Reads the Launcher's settings (the binding with them) and joins Wi-Fi, or joins the firmware's
// connection. The settings, Wi-Fi passwords included, are wiped again before it returns.
Joined Join(Net::LauncherAccount& account)
{
    SetState(State::Connecting);
    void* block = SlowAlloc(sizeof(Net::LauncherSettings));
    if (block == nullptr) return Joined::Retry;
    auto* settings = new (block) Net::LauncherSettings();
    Joined joined = Joined::Retry;
    const Net::SettingsResult read = Net::ReadLauncherSettings(*settings);
    if (read != Net::SettingsResult::Ok)
    {
        ESP_LOGW(kTag, "the Launcher's settings cannot be read");
        joined = Joined::Idle;
    }
    else if (!UsableBinding(settings->account, settings->receiveFirmware))
    {
        joined = Joined::Idle;
    }
    else
    {
        Wipe(account.token, sizeof(account.token));
        account = settings->account;
        Net::ConnectReport report;
        const Net::ConnectStatus status =
            Net::Internal::Connect(User::Presence, *settings, report, kConnectTimeoutMs, Cancelled, nullptr);
        joined = status == Net::ConnectStatus::Connected   ? Joined::Yes
                 : status == Net::ConnectStatus::NoNetworks ? Joined::Idle
                 : status == Net::ConnectStatus::Cancelled  ? Joined::Stopped
                                                            : Joined::Retry;
    }
    settings->Forget();
    Wipe(settings, sizeof(Net::LauncherSettings));
    settings->~LauncherSettings();
    heap_caps_free(block);
    return joined;
}

// One request with the device token. `answer` gets the body (truncated to its capacity). The status is
// Net's; response.status the HTTP status.
Net::SendStatus Ask(const Net::LauncherAccount& account, const char* method, const char* url, const char* body,
                    char* answer, std::size_t capacity, Net::Response& response)
{
    char authorization[sizeof(account.token) + 8];
    std::snprintf(authorization, sizeof(authorization), "Bearer %s", account.token);
    const Net::Header headers[] = {
        {"Authorization", authorization},
        {"Accept", "application/json"},
        {"User-Agent", "CardenzaHAL-presence"},
        {"Content-Type", "application/json"},
    };
    Net::Request request;
    request.method = method;
    request.url = url;
    request.headers = headers;
    request.headerCount = body != nullptr ? 4 : 3;
    request.body = body;
    request.bodyLength = body != nullptr ? std::strlen(body) : 0;
    request.timeoutMs = kRequestTimeoutMs;
    response = Net::Response{};
    response.body = answer;
    response.capacity = capacity;
    const Net::SendStatus status = Net::Internal::Send(User::Presence, request, response, nullptr, nullptr, nullptr);
    Wipe(authorization, sizeof(authorization));
    return status;
}

// POST /launcher/commands/<id>. The HTTP status, 0 when no answer came.
int Report(const Net::LauncherAccount& account, const char* id, const char* status, const char* error)
{
    char url[sizeof(account.api) + 64];
    char body[192];
    if (!Logic::CommandUrl(account.api, id, url, sizeof(url)) ||
        Logic::ReportBody(status, error, body, sizeof(body)) == 0)
        return 0;
    char answer[256];
    Net::Response response;
    const Net::SendStatus sent = Ask(account, "POST", url, body, answer, sizeof(answer), response);
    if (sent != Net::SendStatus::Ok) return 0;
    if (response.status != 200)
        ESP_LOGW(kTag, "command %s: reporting %s answered %d", id, status, response.status);
    return response.status;
}

// The inbox brought commands: refuse what cannot be done, hand the first one over to the Launcher.
// Does not return when it hands over.
void Handle(const Logic::Inbox& inbox, const Net::LauncherAccount& account)
{
    if (inbox.dropped != 0)
        ESP_LOGW(kTag, "%u command(s) in the answer not taken", static_cast<unsigned>(inbox.dropped));
    const Logic::Plan plan = Logic::PlanCommands(inbox);
    for (std::size_t i = 0; i < inbox.count; ++i)
    {
        if (plan.fate[i] == Logic::Fate::HandOver) continue;
        ESP_LOGW(kTag, "command %s refused: %s", inbox.commands[i].id, Logic::RefusalText(plan.fate[i]));
        Report(account, inbox.commands[i].id, "failed", Logic::RefusalText(plan.fate[i]));
    }
    if (plan.handOver < 0) return;
    const Logic::Command& command = inbox.commands[plan.handOver];
    ESP_LOGI(kTag, "command %s: %s \"%s\" %s", command.id, command.Action(), command.firmwareName, command.version);

    HandoverHook hook;
    void* context;
    {
        std::lock_guard<std::mutex> lock(g_hookMutex);
        hook = g_hook;
        context = g_hookContext;
    }
    if (hook != nullptr)
    {
        const Handover handover{command.id, command.start, command.firmwareName, command.version};
        if (!hook(handover, context))
        {
            ESP_LOGW(kTag, "command %s refused by the firmware", command.id);
            Report(account, command.id, "failed", Logic::kRefusedByFirmware);
            return;
        }
    }

    SetState(State::HandingOver);
    Net::Internal::PendingCommand pending;
    pending.command = command.id;
    pending.action = command.Action();
    pending.unlock = command.unlock;
    pending.api = account.api;
    if (!Net::Internal::WritePending(pending))
    {
        Report(account, command.id, "failed", Logic::kHandoverFailed);
        SetState(State::Online);
        return;
    }
    const int answered = Report(account, command.id, "handover", nullptr);
    if (answered == 404 || answered == 409)
    {
        // Finished or expired meanwhile: nothing for the Launcher to do.
        ESP_LOGW(kTag, "command %s is gone (%d): not handed over", command.id, answered);
        Net::Internal::ClearPending();
        SetState(State::Online);
        return;
    }
    // Told, or no answer: the Launcher asks the community for the command either way.
    ESP_LOGW(kTag, "command %s handed over to the Launcher", command.id);
    Net::Internal::Disconnect(User::Presence);
    System::RestartIntoLauncher();
}

bool RoomForRequest()
{
    const bool tlsInPsram = kTlsCanUsePsram && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= 64 * 1024;
    return Logic::MemoryAllows(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                               heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), tlsInPsram);
}

// The service until it stops: switched off, nothing to do, or signed out.
void Run()
{
    if (!g_delayed)
    {
        g_delayed = true;
        if (!Wait(kStartDelayMs)) return;
    }
    Net::LauncherAccount account;
    Logic::Backoff backoff;
    Logic::AudioPause audioPause;
    Audio::TxCounters lastAudio = Audio::GetTxCounters();
    uint32_t waitMs = 0;
    bool retriedToken = false;
    bool stackLogged = false;
    bool gaveWayToAudio = false;
    bool gaveWayToRam = false;

    while (!Stopping())
    {
        if (waitMs != 0 && !Wait(waitMs)) break;
        waitMs = 0;

        if (!Net::Internal::Holds(User::Presence) || !Net::Connected())
        {
            // Wi-Fi joined (or rejoined), and the binding read with the settings.
            const Joined joined = Join(account);
            if (joined == Joined::Idle)
            {
                SetState(State::Idle);
                break;
            }
            if (joined == Joined::Stopped) break;
            if (joined == Joined::Retry)
            {
                SetState(State::Waiting);
                waitMs = backoff.Next() * 1000;
                continue;
            }
        }

        // Giving way: to audio that is already running late, to a firmware short of RAM, to the
        // firmware's own request.
        const Audio::TxCounters audioNow = Audio::GetTxCounters();
        const Logic::Audio audio =
            Logic::ClassifyAudio(audioNow.sent - lastAudio.sent, audioNow.empty - lastAudio.empty);
        lastAudio = audioNow;
        if (audio == Logic::Audio::Struggling)
        {
            if (!gaveWayToAudio) ESP_LOGI(kTag, "the audio is running late: waiting");
            gaveWayToAudio = true;
            SetState(State::Waiting);
            waitMs = kGiveWayMs;
            continue;
        }
        gaveWayToAudio = false;
        if (!RoomForRequest())
        {
            if (!gaveWayToRam) ESP_LOGI(kTag, "not enough internal RAM for a request: waiting");
            gaveWayToRam = true;
            SetState(State::Waiting);
            waitMs = kGiveWayMs;
            continue;
        }
        gaveWayToRam = false;
        if (Net::Internal::Sending(User::Firmware))
        {
            waitMs = kGiveWayMs;
            continue;
        }

        char url[sizeof(account.api) + 40];
        if (!Logic::InboxUrl(account.api, url, sizeof(url)))
        {
            SetState(State::Idle);
            break;
        }
        char* answer = static_cast<char*>(SlowAlloc(kAnswerBytes));
        if (answer == nullptr)
        {
            SetState(State::Waiting);
            waitMs = kGiveWayMs;
            continue;
        }
        Net::Response response;
        const Audio::TxCounters before = Audio::GetTxCounters();
        const Net::SendStatus sent = Ask(account, "GET", url, nullptr, answer, kAnswerBytes, response);
        const Audio::TxCounters after = Audio::GetTxCounters();
        lastAudio = after;
        if (Logic::ClassifyAudio(after.sent - before.sent, after.empty - before.empty) == Logic::Audio::Struggling)
        {
            audioPause.Underrun();
            ESP_LOGW(kTag, "the audio ran late during a request: next in %u s or later",
                     static_cast<unsigned>(audioPause.seconds));
        }
        else
        {
            audioPause.Clean();
        }
        if (Stopping())
        {
            heap_caps_free(answer);
            break;
        }
        if (sent != Net::SendStatus::Ok)
        {
            heap_caps_free(answer);
            SetState(State::Waiting);
            waitMs = std::max(backoff.Next(), audioPause.seconds) * 1000;
            continue;
        }

        char code[32];
        Logic::ErrorCode(answer, response.length, code, sizeof(code));
        const Logic::Verdict verdict = Logic::Classify(response.status, code);
        if (verdict == Logic::Verdict::SignedOut)
        {
            heap_caps_free(answer);
            // A token the Launcher replaced (rotated, bound again) is read once more before giving up.
            Net::LauncherAccount fresh;
            bool receive = true;
            const bool changed = !retriedToken &&
                                 Net::Internal::ReadAccount(fresh, receive) == Net::SettingsResult::Ok &&
                                 UsableBinding(fresh, receive) && std::strcmp(fresh.token, account.token) != 0;
            retriedToken = true;
            if (changed)
            {
                Wipe(account.token, sizeof(account.token));
                account = fresh;
                Wipe(fresh.token, sizeof(fresh.token));
                continue;
            }
            Wipe(fresh.token, sizeof(fresh.token));
            ESP_LOGW(kTag, "the community refused the device token (%d %s): not asking again until a restart",
                     response.status, code);
            g_signedOut = true;
            SetState(State::SignedOut);
            break;
        }
        retriedToken = false;
        // Kept off the stack: the answer's commands (about 1 KB).
        static Logic::Inbox inbox;
        if (verdict != Logic::Verdict::Ok || response.truncated || !Logic::ParseInbox(answer, response.length, inbox))
        {
            heap_caps_free(answer);
            ESP_LOGW(kTag, "inbox: %d%s%s", response.status, code[0] != 0 ? " " : "", code);
            SetState(State::Waiting);
            waitMs = std::max(backoff.Next(), audioPause.seconds) * 1000;
            continue;
        }
        heap_caps_free(answer);
        backoff.Reset();
        SetState(State::Online);
        if (inbox.count != 0 || inbox.dropped != 0) Handle(inbox, account);
        for (Logic::Command& command : inbox.commands) Wipe(command.unlock, sizeof(command.unlock));
        if (!stackLogged)
        {
            stackLogged = true;
            ESP_LOGI(kTag, "stack left unused after a request: %u bytes",
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        }
        waitMs = std::max(inbox.pollSeconds, audioPause.seconds) * 1000;
    }
    Wipe(account.token, sizeof(account.token));
}

void StartTaskLocked();

void TaskMain(void*)
{
    Run();
    Net::Internal::Disconnect(User::Presence);
    std::lock_guard<std::mutex> lock(g_lifecycle);
    const State state = static_cast<State>(g_state.load());
    if (state != State::Idle && state != State::SignedOut) SetState(State::Off);
    g_task = nullptr;
    g_running = false;
    // Switched on again while it was stopping: start over.
    if (g_enabled && state != State::Idle && state != State::SignedOut) StartTaskLocked();
    vTaskDelete(nullptr);
}

void StartTaskLocked()
{
    if (!g_started || !g_enabled || g_running || g_signedOut) return;
    g_running = true;
    if (xTaskCreatePinnedToCore(TaskMain, "presence", kStackBytes, nullptr, kPriority, &g_task, kCore) != pdPASS)
    {
        g_running = false;
        g_task = nullptr;
        ESP_LOGE(kTag, "no memory for the service's task");
    }
}

}  // namespace

void Start()
{
    std::lock_guard<std::mutex> lock(g_lifecycle);
    if (g_started) return;
    g_started = true;
    StartTaskLocked();
}

void Enable(bool enabled)
{
    g_enabled = enabled;
    TaskHandle_t task = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lifecycle);
        if (enabled)
        {
            StartTaskLocked();
            return;
        }
        task = g_task;
    }
    // From the hook (the service's own task): it stops when the hook returns.
    if (task == nullptr || task == xTaskGetCurrentTaskHandle()) return;
    xTaskNotifyGive(task);
    while (g_running) vTaskDelay(pdMS_TO_TICKS(10));
}

bool Enabled()
{
    return g_enabled;
}

State GetState()
{
    return static_cast<State>(g_state.load());
}

void SetHandoverHook(HandoverHook hook, void* context)
{
    std::lock_guard<std::mutex> lock(g_hookMutex);
    g_hook = hook;
    g_hookContext = context;
}

}  // namespace CardenzaHAL::Presence

#else  // CONFIG_CARDENZA_HAL_PRESENCE off: nothing of the service (or of Net) is linked.

namespace CardenzaHAL::Presence {

void Start() {}
void Enable(bool) {}
bool Enabled()
{
    return false;
}
State GetState()
{
    return State::Off;
}
void SetHandoverHook(HandoverHook, void*) {}

}  // namespace CardenzaHAL::Presence

#endif
