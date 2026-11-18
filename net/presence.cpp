// The community presence service (net/presence.h): a low-priority task that keeps the device reachable
// from the 203 community while any firmware runs, and hands commands over to the Launcher. Its
// decisions are in net/presence_logic.cpp (Decide and the rest, tested on the host); this file is the
// task around them.
#include "net/presence.h"

#include <sdkconfig.h>

#if CONFIG_CARDENZA_HAL_PRESENCE

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

#include <esp_app_desc.h>
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
// TLS's handshake runs on this stack (6-8 KB, Net's numbers); the high-water mark is logged once. The
// task reads and writes NVS (the Launcher's settings, the handover), which stops the flash cache: its
// stack has to be internal RAM (xTaskCreatePinnedToCore's default).
constexpr uint32_t kStackBytes = 8192;
constexpr UBaseType_t kPriority = 1;  // above idle only: the firmware's tasks always come first
constexpr BaseType_t kCore = 0;       // beside Wi-Fi's and lwIP's tasks
constexpr std::size_t kAnswerBytes = 16 * 1024;
constexpr uint32_t kRequestTimeoutMs = 10000;
constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr uint32_t kGiveWayMs = 5000;
// Waiting for the firmware's activity to change: SetActivity wakes the service; this is a safety net.
constexpr uint32_t kActivityWaitMs = 30000;
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
std::atomic<uint8_t> g_wait{static_cast<uint8_t>(Wait::None)};
std::atomic<uint8_t> g_activity{static_cast<uint8_t>(Activity::Idle)};
std::atomic<bool> g_wake{false};  // the activity changed: look again now
std::atomic<uint32_t> g_floor{static_cast<uint32_t>(Logic::kDefaultRamFloor)};
std::atomic<int> g_planned{-1};  // Planned(): -1 not read yet, else 0 / 1
std::mutex g_plannedMutex;
TaskHandle_t g_task = nullptr;  // under g_lifecycle
bool g_delayed = false;         // the start delay is done (the task's own)
// The task ended while the firmware's activity kept Wi-Fi down (Run, Park): it starts again when the
// activity changes, and what it learnt carries over.
std::atomic<bool> g_parked{false};
std::size_t g_wifiCost = Logic::kWifiCostEstimate;  // the task's own (Run): learnt when Wi-Fi comes up
bool g_joinedOnce = false;                          // the task's own: the first join of a run is news

// The report (GetReport), 32-bit words: written by the service's task, read by anyone.
std::atomic<uint32_t> g_asked{0};
std::atomic<uint32_t> g_failed{0};
std::atomic<uint32_t> g_joins{0};
std::atomic<uint32_t> g_letGo{0};
std::atomic<uint32_t> g_wifiBytes{0};
std::atomic<uint32_t> g_lastAskMs{0};
std::atomic<uint32_t> g_freeInternal{0};

std::mutex g_hookMutex;
HandoverHook g_hook = nullptr;
void* g_hookContext = nullptr;

// What the requests say about the firmware (Logic::HeaderText), made once by the service's task.
char g_firmwareName[Logic::kHeaderTextMax + 1];
char g_firmwareVersion[Logic::kHeaderTextMax + 1];
char g_imageDigest[65];

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

const char* Name(Wait wait)
{
    switch (wait)
    {
    case Wait::None: return "-";
    case Wait::Activity: return "the firmware's activity";
    case Wait::AudioLate: return "the audio running late";
    case Wait::Ram: return "internal RAM";
    case Wait::FirmwareRequest: return "the firmware's own request";
    case Wait::Failure: return "a failure";
    }
    return "?";
}

// State changes are news for a firmware's log only where something happened that would explain a
// problem: the end of the service (idle, signed out), the handover, the first time online in a run.
// The rest (online again after the firmware let it go, waiting for it) is debug.
void SetState(State state)
{
    static bool wasOnline = false;
    const auto previous = static_cast<State>(g_state.exchange(static_cast<uint8_t>(state)));
    if (state != State::Waiting) g_wait = static_cast<uint8_t>(Wait::None);
    if (previous == state) return;
    const bool news = state == State::Idle || state == State::SignedOut || state == State::HandingOver ||
                      (state == State::Online && !wasOnline);
    if (state == State::Online) wasOnline = true;
    if (news)
        ESP_LOGI(kTag, "%s", Name(state));
    else
        ESP_LOGD(kTag, "%s", Name(state));
}

void SetWaiting(Wait reason)
{
    const auto previous = static_cast<Wait>(g_wait.exchange(static_cast<uint8_t>(reason)));
    if (previous != reason) ESP_LOGD(kTag, "waiting for %s", Name(reason));
    g_state = static_cast<uint8_t>(State::Waiting);
}

void Wipe(void* data, std::size_t size)
{
    volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) bytes[i] = 0;
}

Activity CurrentActivity()
{
    return static_cast<Activity>(g_activity.load());
}

bool Stopping()
{
    return !g_enabled.load();
}

// Connections and requests stop when the service is switched off, or when the firmware wants Wi-Fi gone.
bool Cancelled(void*)
{
    return Stopping() || CurrentActivity() == Activity::Quiet;
}

uint32_t MillisSince(int64_t startUs)
{
    return static_cast<uint32_t>((esp_timer_get_time() - startUs) / 1000);
}

// Waits `ms`; Enable(false) and SetActivity wake it. False when switched off.
bool WaitFor(uint32_t ms)
{
    const int64_t endUs = esp_timer_get_time() + int64_t{ms} * 1000;
    while (!Stopping())
    {
        if (g_wake.exchange(false)) return true;
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

// Whether there is a reason not to use this binding (logged when `log`).
bool UsableBinding(const Net::LauncherAccount& account, bool receiveFirmware, bool log)
{
    if (!account.Bound())
    {
        if (log) ESP_LOGI(kTag, "the Launcher has not bound this device to the community");
        return false;
    }
    if (!receiveFirmware)
    {
        if (log) ESP_LOGI(kTag, "Receive firmware is off in the Launcher");
        return false;
    }
    if (!Logic::AllowedApi(account.api, kLocalHttp))
    {
        // The token goes only to the community's own hosts, over https.
        if (log) ESP_LOGW(kTag, "the binding names a community the device token is not sent to");
        return false;
    }
    return true;
}

// The Launcher's settings, read (read-only) into a block of PSRAM when there is some; null when they
// cannot be read or there is no memory. FreeSettings forgets the passwords, wipes and frees them.
Net::LauncherSettings* ReadSettings(bool quiet)
{
    void* block = SlowAlloc(sizeof(Net::LauncherSettings));
    if (block == nullptr) return nullptr;
    auto* settings = new (block) Net::LauncherSettings();
    if (Net::Internal::ReadSettings(*settings, quiet) == Net::SettingsResult::Ok) return settings;
    settings->~LauncherSettings();
    heap_caps_free(block);
    return nullptr;
}

void FreeSettings(Net::LauncherSettings* settings)
{
    if (settings == nullptr) return;
    settings->Forget();
    Wipe(settings, sizeof(Net::LauncherSettings));
    settings->~LauncherSettings();
    heap_caps_free(settings);
}

// The service has something to do in this run with these settings.
bool UsableSettings(const Net::LauncherSettings& settings, bool log)
{
    if (!UsableBinding(settings.account, settings.receiveFirmware, log)) return false;
    if (settings.networkCount == 0)
    {
        if (log) ESP_LOGI(kTag, "the Launcher has no saved Wi-Fi network");
        return false;
    }
    return true;
}

enum class Joined : uint8_t { Yes, Idle, Retry, Cancelled };

// Reads the Launcher's settings (the binding with them) and joins Wi-Fi, or joins the firmware's
// connection. The settings, Wi-Fi passwords included, are wiped again before it returns.
Joined Join(Net::LauncherAccount& account, bool quiet)
{
    SetState(State::Connecting);
    Net::LauncherSettings* settings = ReadSettings(quiet);
    if (settings == nullptr)
    {
        ESP_LOGW(kTag, "the Launcher's settings cannot be read");
        return Joined::Idle;
    }
    Joined joined = Joined::Idle;
    if (UsableSettings(*settings, true))
    {
        Wipe(account.token, sizeof(account.token));
        account = settings->account;
        Net::ConnectReport report;
        const Net::ConnectStatus status =
            Net::Internal::Connect(User::Presence, *settings, report, kConnectTimeoutMs, Cancelled, nullptr, quiet);
        joined = status == Net::ConnectStatus::Connected   ? Joined::Yes
                 : status == Net::ConnectStatus::NoNetworks ? Joined::Idle
                 : status == Net::ConnectStatus::Cancelled  ? Joined::Cancelled
                                                            : Joined::Retry;
    }
    FreeSettings(settings);
    return joined;
}

// The firmware's name, version and image digest for the requests' headers.
void DescribeFirmware()
{
    const esp_app_desc_t* app = esp_app_get_description();
    Logic::HeaderText(app != nullptr ? app->project_name : nullptr, g_firmwareName, sizeof(g_firmwareName));
    Logic::HeaderText(app != nullptr ? app->version : nullptr, g_firmwareVersion, sizeof(g_firmwareVersion));
    if (!Net::RunningImageDigest(g_imageDigest)) g_imageDigest[0] = 0;
}

// One request with the device token. `answer` gets the body (truncated to its capacity). The status is
// Net's; response.status the HTTP status.
Net::SendStatus Ask(const Net::LauncherAccount& account, const char* method, const char* url, const char* body,
                    char* answer, std::size_t capacity, Net::Response& response)
{
    char authorization[sizeof(account.token) + 8];
    std::snprintf(authorization, sizeof(authorization), "Bearer %s", account.token);
    Net::Header headers[7];
    std::size_t count = 0;
    headers[count++] = {"Authorization", authorization};
    headers[count++] = {"Accept", "application/json"};
    headers[count++] = {"User-Agent", "CardenzaHAL-presence"};
    if (g_firmwareName[0] != 0) headers[count++] = {Logic::kFirmwareNameHeader, g_firmwareName};
    if (g_firmwareVersion[0] != 0) headers[count++] = {Logic::kFirmwareVersionHeader, g_firmwareVersion};
    if (g_imageDigest[0] != 0) headers[count++] = {Logic::kImageDigestHeader, g_imageDigest};
    if (body != nullptr) headers[count++] = {"Content-Type", "application/json"};
    Net::Request request;
    request.method = method;
    request.url = url;
    request.headers = headers;
    request.headerCount = count;
    request.body = body;
    request.bodyLength = body != nullptr ? std::strlen(body) : 0;
    request.timeoutMs = kRequestTimeoutMs;
    response = Net::Response{};
    response.body = answer;
    response.capacity = capacity;
    // A handover's reports are not cancelled by the firmware's activity: they finish what began.
    const bool report = body != nullptr;
    const Net::SendStatus status =
        Net::Internal::Send(User::Presence, request, response, nullptr, report ? nullptr : Cancelled, nullptr);
    Wipe(authorization, sizeof(authorization));
    return status;
}

// POST /launcher/commands/<id>. The HTTP status, 0 when no answer came.
int ReportCommand(const Net::LauncherAccount& account, const char* id, const char* status, const char* error)
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
        ReportCommand(account, inbox.commands[i].id, "failed", Logic::RefusalText(plan.fate[i]));
    }
    if (plan.handOver < 0) return;
    const Logic::Command& command = inbox.commands[plan.handOver];
    ESP_LOGI(kTag, "command %s: %s \"%s\" %s", command.id, command.ActionName(), command.firmwareName, command.version);

    HandoverHook hook;
    void* context;
    {
        std::lock_guard<std::mutex> lock(g_hookMutex);
        hook = g_hook;
        context = g_hookContext;
    }
    if (hook != nullptr)
    {
        const Handover handover{command.id,
                                command.ActionName(),
                                command.action == Logic::Action::InstallStart,
                                command.action == Logic::Action::UpdateLauncher,
                                command.firmwareName,
                                command.version};
        if (!hook(handover, context))
        {
            ESP_LOGW(kTag, "command %s refused by the firmware", command.id);
            ReportCommand(account, command.id, "failed", Logic::kRefusedByFirmware);
            return;
        }
    }

    SetState(State::HandingOver);
    Net::Internal::PendingCommand pending;
    pending.command = command.id;
    pending.action = command.ActionName();
    pending.unlock = command.unlock;
    pending.api = account.api;
    if (!Net::Internal::WritePending(pending))
    {
        ReportCommand(account, command.id, "failed", Logic::kHandoverFailed);
        SetState(State::Online);
        return;
    }
    const int answered = ReportCommand(account, command.id, "handover", nullptr);
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

// What the service knows now (Logic::Decide's view of it).
Logic::Situation Look(Audio::TxCounters& lastAudio, bool ramPaused, std::size_t wifiCost)
{
    Logic::Situation s;
    s.activity = CurrentActivity();
    const Audio::TxCounters audioNow = Audio::GetTxCounters();
    s.audio = Logic::ClassifyAudio(audioNow.sent - lastAudio.sent, audioNow.empty - lastAudio.empty);
    lastAudio = audioNow;
    s.connected = Net::Internal::Holds(User::Presence) && Net::Connected();
    s.firmwareSending = Net::Internal::Sending(User::Firmware);
    s.tlsInPsram = kTlsCanUsePsram && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= 64 * 1024;
    s.wifiInPsram = s.tlsInPsram && Net::Internal::WifiInPsram();
    s.ramPaused = ramPaused;
    s.freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s.largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s.floor = g_floor.load();
    s.wifiCost = wifiCost;
    g_freeInternal = static_cast<uint32_t>(s.freeInternal);
    return s;
}

// The service until it stops: switched off, nothing to do, or signed out.
void Run()
{
    if (!g_delayed)
    {
        g_delayed = true;
        if (!WaitFor(kStartDelayMs)) return;
    }
    DescribeFirmware();
    Net::LauncherAccount account;
    Logic::Backoff backoff;
    Logic::AudioPause audioPause;
    Logic::RamPause ramPause;
    Audio::TxCounters lastAudio = Audio::GetTxCounters();
    std::size_t& wifiCost = g_wifiCost;
    int64_t nextAskUs = 0;      // the poll interval since the last answer
    int64_t nextJoinUs = 0;     // the backoff after a failed join
    int64_t ramPauseUntilUs = 0;
    uint32_t waitMs = 0;
    bool retriedToken = false;
    bool stackLogged = false;
    bool& joinedOnce = g_joinedOnce;
    Wait loggedWait = Wait::None;
    int64_t ramLoggedUs = INT64_MIN / 2;  // RAM waits are news at most every 10 minutes

    while (!Stopping())
    {
        if (waitMs != 0 && !WaitFor(waitMs)) break;
        waitMs = 0;
        // Wi-Fi up but without an address (the network went away): its RAM is given back first, so that
        // joining again is decided like the first time.
        if (Net::Internal::Holds(User::Presence) && !Net::Connected()) Net::Internal::Disconnect(User::Presence);
        const int64_t now = esp_timer_get_time();
        const Logic::Situation s = Look(lastAudio, now < ramPauseUntilUs, wifiCost);
        const Logic::Decision decision = Logic::Decide(s);

        if (decision.step == Logic::Step::LetGo)
        {
            Net::Internal::Disconnect(User::Presence);
            ++g_letGo;
            if (decision.reason == Wait::Ram)
            {
                ramPause.LetGo();
                ramPauseUntilUs = esp_timer_get_time() + int64_t{ramPause.seconds} * 1000000;
                ESP_LOGI(kTag, "Wi-Fi let go: the firmware needs its internal RAM (%u free, floor %u); again in %u s or later",
                         static_cast<unsigned>(s.freeInternal), static_cast<unsigned>(s.floor),
                         static_cast<unsigned>(ramPause.seconds));
            }
            SetWaiting(decision.reason);
            continue;
        }
        if (decision.step == Logic::Step::Wait)
        {
            SetWaiting(decision.reason);
            // The firmware's activity keeps Wi-Fi down (Quiet, or RealTime while it is down): the task ends
            // and gives its stack (8 KB of internal RAM) back to the firmware, which wants it most then;
            // SetActivity starts it again.
            if (decision.reason == Wait::Activity && !s.connected)
            {
                g_parked = true;
                break;
            }
            // RAM is news once per stretch (and at most every 10 minutes): it is why a device does not
            // come online.
            if (decision.reason == Wait::Ram && loggedWait != Wait::Ram && !s.ramPaused &&
                now - ramLoggedUs >= int64_t{600} * 1000000)
            {
                ramLoggedUs = now;
                ESP_LOGI(kTag, "waiting for internal RAM: %u free (largest %u); %s needs %u beside the firmware's %u",
                         static_cast<unsigned>(s.freeInternal), static_cast<unsigned>(s.largestInternal),
                         s.connected ? "a request" : "Wi-Fi",
                         static_cast<unsigned>(s.connected ? (!s.tlsInPsram   ? Logic::kRequestBytesTlsInternal
                                                              : s.wifiInPsram ? Logic::kRequestBytesAllInPsram
                                                                              : Logic::kRequestBytesTlsInPsram)
                                                           : wifiCost),
                         static_cast<unsigned>(s.floor));
            }
            if (decision.reason == Wait::AudioLate && loggedWait != Wait::AudioLate)
                ESP_LOGI(kTag, "the audio is running late: waiting");
            loggedWait = decision.reason;
            waitMs = decision.reason == Wait::Activity ? kActivityWaitMs : kGiveWayMs;
            continue;
        }
        loggedWait = Wait::None;

        if (decision.step == Logic::Step::Join)
        {
            if (now < nextJoinUs)
            {
                SetWaiting(Wait::Failure);
                waitMs = static_cast<uint32_t>((nextJoinUs - now) / 1000) + 1;
                continue;
            }
            const std::size_t freeBefore = s.freeInternal;
            const Joined joined = Join(account, joinedOnce);
            if (joined == Joined::Idle)
            {
                SetState(State::Idle);
                break;
            }
            if (joined == Joined::Cancelled) continue;  // Quiet came, or switched off: look again
            if (joined == Joined::Retry)
            {
                ++g_failed;
                nextJoinUs = esp_timer_get_time() + int64_t{backoff.Next()} * 1000000;
                SetWaiting(Wait::Failure);
                continue;
            }
            ++g_joins;
            const std::size_t freeAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            wifiCost = Logic::LearnWifiCost(wifiCost, freeBefore, freeAfter);
            g_wifiBytes = static_cast<uint32_t>(freeBefore > freeAfter ? freeBefore - freeAfter : 0);
            if (!joinedOnce)
                ESP_LOGI(kTag, "Wi-Fi up: it took %u of internal RAM (%u free now)",
                         static_cast<unsigned>(g_wifiBytes.load()), static_cast<unsigned>(freeAfter));
            joinedOnce = true;
            nextAskUs = 0;
            continue;
        }

        // Ask, once the poll interval has passed (an activity change wakes the service early).
        if (now < nextAskUs)
        {
            g_state = static_cast<uint8_t>(State::Online);
            waitMs = static_cast<uint32_t>((nextAskUs - now) / 1000) + 1;
            continue;
        }
        char url[sizeof(account.api) + 40];
        if (!Logic::InboxUrl(account.api, url, sizeof(url)))
        {
            SetState(State::Idle);
            break;
        }
        // The answer and its commands in PSRAM when there is some (16 KB and about 1 KB).
        void* block = SlowAlloc(kAnswerBytes + sizeof(Logic::Inbox));
        if (block == nullptr)
        {
            SetWaiting(Wait::Ram);
            waitMs = kGiveWayMs;
            continue;
        }
        char* answer = static_cast<char*>(block);
        auto* inbox = new (static_cast<char*>(block) + kAnswerBytes) Logic::Inbox();
        Net::Response response;
        const Audio::TxCounters before = Audio::GetTxCounters();
        const int64_t askStartUs = esp_timer_get_time();
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
        const auto freeBlock = [&] {
            for (Logic::Command& command : inbox->commands) Wipe(command.unlock, sizeof(command.unlock));
            inbox->~Inbox();
            heap_caps_free(block);
        };
        if (Stopping())
        {
            freeBlock();
            break;
        }
        if (sent == Net::SendStatus::Cancelled)
        {
            freeBlock();  // Quiet came: the next look lets go
            continue;
        }
        if (sent != Net::SendStatus::Ok)
        {
            freeBlock();
            ++g_failed;
            SetWaiting(Wait::Failure);
            nextAskUs = esp_timer_get_time() + int64_t{std::max(backoff.Next(), audioPause.seconds)} * 1000000;
            continue;
        }

        char code[32];
        Logic::ErrorCode(answer, response.length, code, sizeof(code));
        const Logic::Verdict verdict = Logic::Classify(response.status, code);
        if (verdict == Logic::Verdict::SignedOut)
        {
            freeBlock();
            // A token the Launcher replaced (rotated, bound again) is read once more before giving up.
            Net::LauncherAccount fresh;
            bool receive = true;
            const bool changed = !retriedToken &&
                                 Net::Internal::ReadAccount(fresh, receive) == Net::SettingsResult::Ok &&
                                 UsableBinding(fresh, receive, false) && std::strcmp(fresh.token, account.token) != 0;
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
        if (verdict != Logic::Verdict::Ok || response.truncated || !Logic::ParseInbox(answer, response.length, *inbox))
        {
            ESP_LOGW(kTag, "inbox: %d%s%s", response.status, code[0] != 0 ? " " : "", code);
            freeBlock();
            ++g_failed;
            SetWaiting(Wait::Failure);
            nextAskUs = esp_timer_get_time() + int64_t{std::max(backoff.Next(), audioPause.seconds)} * 1000000;
            continue;
        }
        ++g_asked;
        g_lastAskMs = MillisSince(askStartUs);
        backoff.Reset();
        ramPause.Clean();
        SetState(State::Online);
        if (inbox->count != 0 || inbox->dropped != 0) Handle(*inbox, account);
        nextAskUs = esp_timer_get_time() + int64_t{std::max(inbox->pollSeconds, audioPause.seconds)} * 1000000;
        freeBlock();
        if (!stackLogged)
        {
            stackLogged = true;
            ESP_LOGI(kTag, "first request in %u ms; stack left unused: %u bytes", static_cast<unsigned>(g_lastAskMs.load()),
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        }
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
    const bool parked = g_parked.load();
    if (state != State::Idle && state != State::SignedOut && !parked) SetState(State::Off);
    g_task = nullptr;
    g_running = false;
    // Switched on again while it was stopping, or the activity changed while it parked: start over.
    if (g_enabled && state != State::Idle && state != State::SignedOut && (!parked || g_wake.load())) StartTaskLocked();
    vTaskDelete(nullptr);
}

void StartTaskLocked()
{
    if (!g_started || !g_enabled || g_running || g_signedOut) return;
    g_running = true;
    // Before the task exists: it may park again at once.
    const bool wasParked = g_parked.exchange(false);
    if (xTaskCreatePinnedToCore(TaskMain, "presence", kStackBytes, nullptr, kPriority, &g_task, kCore) != pdPASS)
    {
        g_running = false;
        g_task = nullptr;
        g_parked = wasParked;  // a parked service tries again at the next change of the activity
        ESP_LOGE(kTag, "no memory for the service's task");
    }
}

void Wake()
{
    g_wake = true;
    std::lock_guard<std::mutex> lock(g_lifecycle);
    if (g_task != nullptr)
        xTaskNotifyGive(g_task);
    else if (g_parked)
        StartTaskLocked();  // it ended while the activity kept Wi-Fi down
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

bool Planned()
{
    if (!g_enabled) return false;
    const int known = g_planned.load();
    if (known >= 0) return known != 0;
    std::lock_guard<std::mutex> lock(g_plannedMutex);
    if (g_planned.load() < 0)
    {
        Net::LauncherSettings* settings = ReadSettings(true);
        const bool planned = settings != nullptr && UsableSettings(*settings, false);
        FreeSettings(settings);
        g_planned = planned ? 1 : 0;
    }
    return g_planned.load() != 0;
}

void SetActivity(Activity activity)
{
    if (static_cast<Activity>(g_activity.exchange(static_cast<uint8_t>(activity))) != activity) Wake();
}

Activity GetActivity()
{
    return CurrentActivity();
}

void SetRamFloor(std::size_t bytes)
{
    g_floor = static_cast<uint32_t>(std::min<std::size_t>(bytes, UINT32_MAX));
}

Report GetReport()
{
    Report report;
    report.state = static_cast<State>(g_state.load());
    report.waiting = report.state == State::Waiting ? static_cast<Wait>(g_wait.load()) : Wait::None;
    report.asked = g_asked;
    report.failed = g_failed;
    report.joins = g_joins;
    report.letGo = g_letGo;
    report.wifiBytes = g_wifiBytes;
    report.lastAskMs = g_lastAskMs;
    report.freeInternal = g_freeInternal;
    return report;
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
bool Planned()
{
    return false;
}
void SetActivity(Activity) {}
Activity GetActivity()
{
    return Activity::Idle;
}
void SetRamFloor(std::size_t) {}
Report GetReport()
{
    return Report{};
}
State GetState()
{
    return State::Off;
}
void SetHandoverHook(HandoverHook, void*) {}

}  // namespace CardenzaHAL::Presence

#endif
