#pragma once

#include <cstddef>
#include <cstdint>

// The presence service's decisions (net/presence.h) as plain functions: where the device token may go,
// the inbox answer, which command is handed over, how long to wait, when the audio or the RAM says "not
// now", and what is written for the Launcher. Pure (no ESP-IDF): host tests and simulators include it.
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
    struct Command {
        char id[kIdMax + 1] = {};  // as it goes back into /launcher/commands/<id>: digits, or [A-Za-z0-9_-]
        bool known = false;        // action is install or install_start
        bool start = false;        // install_start: the Launcher starts it after installing
        char unlock[kUnlockLength + 1] = {};  // a private firmware's unlock token, or empty
        char firmwareName[65] = {};           // for the log and the firmware's hook (UTF-8, cut at a character)
        char version[33] = {};
        const char* Action() const { return start ? "install_start" : "install"; }
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

    // --- RAM ------------------------------------------------------------------------------------------

    // Whether a request may start: internal RAM free and its largest block, and whether TLS takes its
    // memory from PSRAM (CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC with PSRAM in the heap). TLS in PSRAM: 40 KB free
    // and a 12 KB block (Wi-Fi's buffers, TCP, the HTTP client); in internal RAM: 80 KB and 24 KB (its 16 KB
    // receive record too). Below that the firmware needs its RAM more than the community needs an answer.
    bool MemoryAllows(std::size_t freeInternal, std::size_t largestInternal, bool tlsInPsram);

    // --- The handover ---------------------------------------------------------------------------------

    // NVS namespace l_pending, written by a firmware for the Launcher (203-launcher
    // docs/SHARED_SETTINGS.md): the command the Launcher carries on after the restart.
    namespace Pending {
        inline constexpr char kNamespace[] = "l_pending";
        inline constexpr char kVersionKey[] = "v";  // u8
        inline constexpr uint8_t kVersion = 1;
        inline constexpr char kCommandKey[] = "cmd";      // string: the command's id
        inline constexpr char kActionKey[] = "action";    // string: install | install_start
        inline constexpr char kUnlockKey[] = "unlock";    // string: "cfu_..." when the inbox gave one
        inline constexpr char kApiKey[] = "api";          // string: the community the command came from
    }

}  // namespace CardenzaHAL::Presence::Logic
