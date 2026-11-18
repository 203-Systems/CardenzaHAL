#pragma once

#include <cstddef>
#include <cstdint>

#include "net/presence.h"

// The presence service's decisions (net/presence.h) as plain functions: where the device token may go,
// the inbox answer, which command is handed over, how long to wait, when the firmware's activity, the
// audio or the RAM says "not now", what the requests say about the firmware, and what is written for the
// Launcher. Pure (no ESP-IDF): host tests and simulators include it.
namespace CardenzaHAL::Presence::Logic {

    // --- Where the device token goes ------------------------------------------------------------------

    // The community hosts the service sends the device token to (https only, port 443).
    inline constexpr const char* kAllowedHosts[] = {"community.203.io", "cardenza.203.io"};

    // Whether the community API base from the Launcher's binding (l_account api) may get the device
    // token: https:// on a host of kAllowedHosts (any case, no port or 443) and a path, as
    // "https://community.203.io/api". With `localHttp` (a debug build's Kconfig option, never a release)
    // also http:// to an IPv4 loopback or private address, for a test community on the developer's LAN.
    bool AllowedApi(const char* api, bool localHttp);

    // <api>/launcher/inbox?from=firmware and <api>/launcher/commands/<id> (a trailing '/' of api is
    // dropped). False when it does not fit.
    bool InboxUrl(const char* api, char* out, std::size_t capacity);
    bool CommandUrl(const char* api, const char* id, char* out, std::size_t capacity);

    // --- The inbox answer -----------------------------------------------------------------------------

    inline constexpr std::size_t kIdMax = 40;
    inline constexpr std::size_t kUnlockLength = 68;  // "cfu_" + 64 lower-case hex digits
    // What a command asks for (COMMUNITY.md, Command.action). The service hands every known one over to
    // the Launcher, which carries it out.
    enum class Action : uint8_t {
        Unknown,         // anything else: refused ("Unsupported command")
        Install,         // install
        InstallStart,    // install_start: the Launcher starts it after installing
        UpdateLauncher,  // update_launcher: the 203 Launcher updates itself
    };
    Action ActionOf(const char* text);
    // The action as the community and l_pending spell it ("" for Unknown); at most kActionMax characters.
    const char* ActionText(Action action);
    inline constexpr std::size_t kActionMax = 15;
    struct Command {
        char id[kIdMax + 1] = {};  // as it goes back into /launcher/commands/<id>: digits, or [A-Za-z0-9_-]
        Action action = Action::Unknown;
        char unlock[kUnlockLength + 1] = {};  // a private firmware's unlock token, or empty
        char firmwareName[65] = {};           // for the log and the firmware's hook (UTF-8, cut at a character)
        char version[33] = {};
        bool Known() const { return action != Action::Unknown; }
        const char* ActionName() const { return ActionText(action); }
    };
    inline constexpr std::size_t kMaxCommands = 4;
    inline constexpr uint32_t kDefaultPollSeconds = 15;
    struct Inbox {
        Command commands[kMaxCommands];
        std::size_t count = 0;
        std::size_t dropped = 0;  // beyond kMaxCommands, or without an id that can be answered
        uint32_t pollSeconds = kDefaultPollSeconds;
    };
    // The answer of GET /launcher/inbox?from=firmware: {"commands":[...],"poll":15}. Unknown keys are
    // skipped, whatever their value. False when it is not that (not JSON, or no "commands" array).
    bool ParseInbox(const char* json, std::size_t length, Inbox& inbox);

    // The "code" of an error answer ({"error":"...","code":"signin_required"}); empty when there is none.
    void ErrorCode(const char* json, std::size_t length, char* code, std::size_t capacity);

    // The poll interval to keep: the answer's, within 5..300 s.
    uint32_t PollSeconds(uint32_t answered);

    // --- What an answer means -------------------------------------------------------------------------

    enum class Verdict : uint8_t {
        Ok,         // 200
        SignedOut,  // the community refused the token: 401 signin_required, or 403 banned
        Retry,      // anything else: try again later
    };
    Verdict Classify(int httpStatus, const char* errorCode);

    // What happens to each command of an answer: the first known one is handed over to the Launcher;
    // every other one is refused at once (reported failed), so the website does not wait for it.
    enum class Fate : uint8_t { HandOver, Unsupported, Busy };
    struct Plan {
        Fate fate[kMaxCommands] = {};
        int handOver = -1;  // index, or -1
    };
    Plan PlanCommands(const Inbox& inbox);
    // The short English error for a refused command (COMMUNITY.md: POST /launcher/commands/<id>).
    const char* RefusalText(Fate fate);
    inline constexpr const char* kRefusedByFirmware = "Refused by the running firmware";
    inline constexpr const char* kHandoverFailed = "Could not hand over to the Launcher";

    // {"status":"handover"} or {"status":"failed","error":"..."} (error: printable ASCII, at most 120
    // characters, escaped). Returns the length, 0 when it does not fit.
    std::size_t ReportBody(const char* status, const char* error, char* out, std::size_t capacity);

    // --- Waiting --------------------------------------------------------------------------------------

    // After failures: 5, 10, 20, 40, 80, 160, then 300 s; back to 5 after a success.
    struct Backoff {
        uint32_t failures = 0;
        uint32_t Next();  // the wait after one more failure, in seconds
        void Reset() { failures = 0; }
    };

    // --- The audio ------------------------------------------------------------------------------------

    // What the speaker's DMA did over a while (audio/audio.h, TxCounters: blocks sent, and blocks that
    // went out with nothing new written).
    enum class Audio : uint8_t {
        Quiet,       // the channel is off, or nothing is being written (three quarters or more of the
                     // blocks empty): no real-time audio to disturb
        Streaming,   // written in time, block after block
        Struggling,  // streaming, with underruns: the device has no time to spare
    };
    Audio ClassifyAudio(uint32_t sentBlocks, uint32_t emptyBlocks);
    // A request during which the audio struggled pauses the service while audio streams: 60 s, doubling
    // with each such request up to 300 s; a request without an underrun ends the pause.
    struct AudioPause {
        uint32_t seconds = 0;
        void Underrun() { seconds = seconds == 0 ? 60 : (seconds >= 150 ? 300 : seconds * 2); }
        void Clean() { seconds = 0; }
    };

    // --- What to do next ------------------------------------------------------------------------------

    // The internal RAM a firmware keeps for itself unless it says otherwise (SetRamFloor).
    inline constexpr std::size_t kDefaultRamFloor = 24 * 1024;
    // What Wi-Fi takes of internal RAM until it has been measured on this device (its buffers, tasks,
    // lwIP, the supplicant; the reference sdkconfig of README.md).
    inline constexpr std::size_t kWifiCostEstimate = 40 * 1024;
    // Wi-Fi's largest block (its task's stack is about 6.5 KB).
    inline constexpr std::size_t kWifiLargestBlock = 8 * 1024;
    // A request's internal RAM beside TLS (TCP and Wi-Fi buffers, the HTTP client's 2 KB): with TLS in
    // PSRAM 12 KB and an 8 KB block; with TLS in internal RAM 48 KB and a 20 KB block (its 16 KB receive
    // record). With Wi-Fi's own allocations in PSRAM too (the frames it copies for lwIP while they come
    // in: Net's adapter table, net.cpp WifiFuncs) 10 KB: measured on a Cardenza, internal RAM went down by
    // 9.5 KB during a full TLS handshake and by 6 KB during a resumed one (CardenzaSynth, 2026-11-18).
    inline constexpr std::size_t kRequestBytesTlsInPsram = 12 * 1024;
    inline constexpr std::size_t kRequestBytesAllInPsram = 10 * 1024;
    inline constexpr std::size_t kRequestBlockTlsInPsram = 8 * 1024;
    inline constexpr std::size_t kRequestBytesTlsInternal = 48 * 1024;
    inline constexpr std::size_t kRequestBlockTlsInternal = 20 * 1024;

    // What the service knows when it looks.
    struct Situation {
        Activity activity = Activity::Idle;
        Audio audio = Audio::Quiet;      // the speaker's DMA since the last look
        bool connected = false;          // the service holds Wi-Fi and it has an address
        bool firmwareSending = false;    // the firmware's own Net request is under way
        bool tlsInPsram = false;         // TLS takes its memory from PSRAM
        bool wifiInPsram = false;        // so do Wi-Fi's own allocations (Net::Internal::WifiInPsram)
        bool ramPaused = false;          // Wi-Fi was let go for the firmware's RAM a short while ago
        std::size_t freeInternal = 0;    // internal RAM free, and its largest block
        std::size_t largestInternal = 0;
        std::size_t floor = kDefaultRamFloor;
        std::size_t wifiCost = kWifiCostEstimate;  // learnt (LearnWifiCost) or the estimate
    };
    enum class Step : uint8_t {
        Ask,    // ask the inbox now
        Join,   // bring Wi-Fi up (and read the Launcher's settings)
        LetGo,  // let go of Wi-Fi: the firmware needs it gone
        Wait,   // nothing now (the reason says why)
    };
    struct Decision {
        Step step = Step::Wait;
        Wait reason = Wait::None;
    };
    // In order: Quiet lets go of Wi-Fi (or waits); a firmware below its RAM floor gets Wi-Fi's RAM back;
    // audio running late waits; without Wi-Fi, RealTime waits, then Wi-Fi comes up when the floor and
    // Wi-Fi's cost fit (not again for a while after a RAM let-go); with Wi-Fi, the firmware's own request
    // waits, then a request starts when the floor and the request's RAM fit.
    Decision Decide(const Situation& situation);

    // What Wi-Fi took: internal RAM free before it came up less what is free once it has an address,
    // within 16..96 KB (other tasks allocate meanwhile too); `previous` when the measure makes no sense.
    std::size_t LearnWifiCost(std::size_t previous, std::size_t freeBefore, std::size_t freeAfter);

    // After Wi-Fi was let go for the firmware's RAM, it does not come up again for a while: 60 s, doubling
    // up to 600 s while the firmware keeps needing it; a quiet stretch (a request without a let-go) ends it.
    struct RamPause {
        uint32_t seconds = 0;
        void LetGo() { seconds = seconds == 0 ? 60 : (seconds >= 300 ? 600 : seconds * 2); }
        void Clean() { seconds = 0; }
    };

    // --- What the requests say about the firmware -----------------------------------------------------

    // Headers on every inbox request, so the community knows what runs (COMMUNITY.md, presence): the
    // firmware's name and version from its app description, and its image digest (the community knows
    // its builds by it). Values: printable ASCII without '"' or a backslash (other bytes become '_'), spaces
    // trimmed, at most 32 characters; empty when nothing is left.
    inline constexpr const char* kFirmwareNameHeader = "X-Firmware-Name";
    inline constexpr const char* kFirmwareVersionHeader = "X-Firmware-Version";
    inline constexpr const char* kImageDigestHeader = "X-Image-Digest";
    inline constexpr std::size_t kHeaderTextMax = 32;
    void HeaderText(const char* text, char* out, std::size_t capacity);

    // Where an ESP-IDF app image keeps its appended SHA-256 (the digest the community knows a build by,
    // what esp_partition_get_sha256 gives for an app partition), found from its headers alone: the 24-byte
    // image header (magic 0xE9, the segment count, byte 23 hash_appended), each segment's 8-byte header
    // and data, the checksum byte padded to 16 (L0 = (end of the last segment + 16) & ~15), the digest at
    // L0. `read` reads `length` bytes at `offset` of the image (false: failed). False for anything that
    // is not such an image within `limit` bytes, or one without an appended digest.
    using ReadAt = bool (*)(void* context, uint32_t offset, void* out, uint32_t length);
    bool AppendedDigestOffset(ReadAt read, void* context, uint32_t limit, uint32_t& offset);

    // --- The handover ---------------------------------------------------------------------------------

    // NVS namespace l_pending, written by a firmware for the Launcher (203-launcher
    // docs/SHARED_SETTINGS.md): the command the Launcher carries on after the restart.
    namespace Pending {
        inline constexpr char kNamespace[] = "l_pending";
        inline constexpr char kVersionKey[] = "v";  // u8
        inline constexpr uint8_t kVersion = 1;
        inline constexpr char kCommandKey[] = "cmd";      // string: the command's id
        inline constexpr char kActionKey[] = "action";    // string: install | install_start | update_launcher
        inline constexpr char kUnlockKey[] = "unlock";    // string: "cfu_..." when the inbox gave one
        inline constexpr char kApiKey[] = "api";          // string: the community the command came from
    }

}  // namespace CardenzaHAL::Presence::Logic
