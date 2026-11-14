// net/presence_logic.h: the presence service's decisions (where the device token goes, the inbox answer,
// the handover plan, backoff, giving way to audio and RAM, the report bodies), and the Launcher's
// restart ticket (system/launcher.h). Pure code, no ESP-IDF.
#include "net/presence_logic.h"
#include "system/launcher.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#define CHECK(condition)                                                                 \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::cerr << __FILE__ << ":" << __LINE__ << ": failed: " #condition << "\n"; \
            std::abort();                                                                \
        }                                                                                \
    } while (0)

namespace {

namespace Logic = CardenzaHAL::Presence::Logic;
namespace Ticket = CardenzaHAL::System::LauncherTicket;

const std::string kUnlock = "cfu_" + std::string(64, 'a');

bool Parse(const std::string& json, Logic::Inbox& inbox)
{
    return Logic::ParseInbox(json.data(), json.size(), inbox);
}

void AllowedApiTests()
{
    CHECK(Logic::AllowedApi("https://community.203.io/api", false));
    CHECK(Logic::AllowedApi("https://cardenza.203.io/community/api", false));
    CHECK(Logic::AllowedApi("https://COMMUNITY.203.io/api/", false));
    CHECK(Logic::AllowedApi("https://community.203.io:443/api", false));
    // Not the community's own host, or not https, or a bare host.
    CHECK(!Logic::AllowedApi("http://community.203.io/api", false));
    CHECK(!Logic::AllowedApi("https://community.203.io.evil.example/api", false));
    CHECK(!Logic::AllowedApi("https://evil.example/community.203.io/api", false));
    CHECK(!Logic::AllowedApi("https://xcommunity.203.io/api", false));
    CHECK(!Logic::AllowedApi("https://community.203.io:8443/api", false));
    CHECK(!Logic::AllowedApi("https://community.203.io", false));
    CHECK(!Logic::AllowedApi("https://community.203.io/", false));
    CHECK(!Logic::AllowedApi("https://user@community.203.io/api", false));
    CHECK(!Logic::AllowedApi("https://community.203.io/api?x=1", false));
    CHECK(!Logic::AllowedApi("", false));
    CHECK(!Logic::AllowedApi(nullptr, false));
    // A test community over http only in a debug build, and only on a loopback or private address.
    CHECK(!Logic::AllowedApi("http://127.0.0.1:8080/api", false));
    CHECK(Logic::AllowedApi("http://127.0.0.1:8080/api", true));
    CHECK(Logic::AllowedApi("http://192.168.1.20/api", true));
    CHECK(Logic::AllowedApi("http://10.0.0.5:3000/api", true));
    CHECK(Logic::AllowedApi("http://172.20.1.1/api", true));
    CHECK(!Logic::AllowedApi("http://172.32.1.1/api", true));
    CHECK(!Logic::AllowedApi("http://8.8.8.8/api", true));
    CHECK(!Logic::AllowedApi("http://example.com/api", true));
    CHECK(!Logic::AllowedApi("http://192.168.01.1/api", true));
    CHECK(Logic::AllowedApi("https://community.203.io/api", true));
}

void UrlTests()
{
    char url[160];
    CHECK(Logic::InboxUrl("https://community.203.io/api", url, sizeof(url)));
    CHECK(std::strcmp(url, "https://community.203.io/api/launcher/inbox?from=firmware") == 0);
    CHECK(Logic::InboxUrl("https://community.203.io/api/", url, sizeof(url)));
    CHECK(std::strcmp(url, "https://community.203.io/api/launcher/inbox?from=firmware") == 0);
    CHECK(Logic::CommandUrl("https://cardenza.203.io/community/api", "123", url, sizeof(url)));
    CHECK(std::strcmp(url, "https://cardenza.203.io/community/api/launcher/commands/123") == 0);
    CHECK(!Logic::CommandUrl("https://community.203.io/api", "1/../x", url, sizeof(url)));
    CHECK(!Logic::CommandUrl("https://community.203.io/api", "", url, sizeof(url)));
    char small[20];
    CHECK(!Logic::InboxUrl("https://community.203.io/api", small, sizeof(small)));
}

void InboxTests()
{
    Logic::Inbox inbox;
    // Nothing to do.
    CHECK(Parse(R"({"commands":[],"poll":15})", inbox));
    CHECK(inbox.count == 0 && inbox.dropped == 0 && inbox.pollSeconds == 15);
    CHECK(Parse(" {\"poll\": 30 ,\n \"commands\" : [ ] } \n", inbox));
    CHECK(inbox.pollSeconds == 30);
    CHECK(Parse(R"({"commands":[]})", inbox) && inbox.pollSeconds == 15);

    // One command as the server sends it (jsonb: keys by length; the Item and everything else skipped).
    const std::string answer =
        R"({"poll": 15, "commands": [{"id": 4211, "item": {"name": "Synth", "size": 1572864, "mirrors": )"
        R"(["https://launcher-cdn.203.io/a.bin", "https://launcher-apps.b-cdn.net/a.bin"], "install": {"app": )"
        R"({"image_size": 1572864, "source_offset": 0}, "partitions": []}, "sha256": "ab", "published": null, )"
        R"("description": "A \"quoted\" \\ text é🎹"}, "action": "install_start", "device": )"
        R"({"id": 7, "name": "Cardenza"}, "error": null, "status": "delivered", "target": "cardenza", "unlock": ")" +
        kUnlock +
        R"(", "created": "2026-11-14T10:00:00Z", "updated": "2026-11-14T10:00:01Z", "version": {"code": "ABCDEF", )"
        R"("channel": "stable", "version": "1.2.0"}, "firmware": {"code": "XYZ234", "name": "Groove\nbox 鸣片"}, )"
        R"("progress": 0}]})";
    CHECK(Parse(answer, inbox));
    CHECK(inbox.count == 1 && inbox.dropped == 0);
    const Logic::Command& command = inbox.commands[0];
    CHECK(std::strcmp(command.id, "4211") == 0);
    CHECK(command.known && command.start);
    CHECK(std::strcmp(command.Action(), "install_start") == 0);
    CHECK(kUnlock == command.unlock);
    CHECK(std::strcmp(command.version, "1.2.0") == 0);
    CHECK(std::strcmp(command.firmwareName, "Groove box \xE9\xB8\xA3\xE7\x89\x87") == 0);  // a control character as a space

    // install, no unlock; a string id; a bad unlock is left out.
    CHECK(Parse(R"({"commands":[{"id":"cmd_9-x","action":"install","unlock":"cfu_short"}],"poll":5})", inbox));
    CHECK(inbox.count == 1 && std::strcmp(inbox.commands[0].id, "cmd_9-x") == 0);
    CHECK(inbox.commands[0].known && !inbox.commands[0].start && std::strcmp(inbox.commands[0].Action(), "install") == 0);
    CHECK(inbox.commands[0].unlock[0] == 0 && inbox.pollSeconds == 5);
    CHECK(Parse(R"({"commands":[{"id":1,"action":"install","unlock":")" + std::string("cfu_") + std::string(64, 'A') +
                    R"("}]})",
                inbox));
    CHECK(inbox.commands[0].unlock[0] == 0);  // upper-case hex: not a token

    // Unknown actions are kept (to be refused); commands without an id that can be answered are dropped.
    CHECK(Parse(R"({"commands":[{"id":2,"action":"erase"},{"action":"install"},{"id":"a/b","action":"install"},)"
                R"({"id":-3,"action":"install"},{"id":1.5,"action":"install"},{"id":null},[1],"x"]})",
                inbox));
    CHECK(inbox.count == 1 && !inbox.commands[0].known && inbox.dropped == 7);
    CHECK(Parse(R"({"commands":[{"id":12345678901234567890123,"action":"install"}]})", inbox));
    CHECK(inbox.count == 0 && inbox.dropped == 1);  // more than 19 digits
    CHECK(Parse(R"({"commands":[{"id":9223372036854775807,"action":"install"}]})", inbox));
    CHECK(inbox.count == 1 && std::strcmp(inbox.commands[0].id, "9223372036854775807") == 0);

    // At most kMaxCommands are kept.
    std::string many = R"({"commands":[)";
    for (int i = 1; i <= 6; ++i) many += (i > 1 ? "," : "") + std::string(R"({"id":)") + std::to_string(i) + R"(,"action":"install"})";
    many += "]}";
    CHECK(Parse(many, inbox));
    CHECK(inbox.count == Logic::kMaxCommands && inbox.dropped == 2);

    // Poll within 5..300; odd values ignored.
    CHECK(Parse(R"({"commands":[],"poll":1})", inbox) && inbox.pollSeconds == 5);
    CHECK(Parse(R"({"commands":[],"poll":100000})", inbox) && inbox.pollSeconds == 300);
    CHECK(Parse(R"({"commands":[],"poll":99999999})", inbox) && inbox.pollSeconds == 15);
    CHECK(Parse(R"({"commands":[],"poll":-5})", inbox) && inbox.pollSeconds == 15);
    CHECK(Parse(R"({"commands":[],"poll":"20"})", inbox) && inbox.pollSeconds == 15);
    CHECK(Parse(R"({"commands":[],"poll":0})", inbox) && inbox.pollSeconds == 15);

    // Not an inbox.
    for (const char* bad : {"", "null", "[]", R"({"poll":15})", R"({"commands":{}})", R"({"commands":[)",
                            R"({"commands":[]} x)", R"({"commands":[{"id":1,}]})", R"({"commands":[],})",
                            R"({"commands":[{"id":1,"action":"install"])", R"({'commands':[]})",
                            "{\"commands\":[{\"id\":\"a\x01\"}]}", R"({"commands":[{"id":"\ud800"}]})",
                            R"({"commands":[],"poll":01})", "{\"commands\":[],\"x\":\"\xC3\"}",
                            "{\"commands\":[],\"x\":\"\xC3\x28\"}"})
    {
        inbox.count = 3;
        CHECK(!Parse(bad, inbox));
        CHECK(inbox.count == 0);
    }
    CHECK(!Logic::ParseInbox(nullptr, 0, inbox));
    // Deep nesting is refused, not followed.
    std::string deep = R"({"commands":[],"x":)" + std::string(40, '[') + std::string(40, ']') + "}";
    CHECK(!Parse(deep, inbox));
    std::string fine = R"({"commands":[],"x":)" + std::string(20, '[') + std::string(20, ']') + "}";
    CHECK(Parse(fine, inbox));
    // A truncated answer (cut by the 16 KB buffer) does not parse.
    CHECK(!Parse(answer.substr(0, answer.size() / 2), inbox));
}

void ErrorTests()
{
    char code[32];
    const std::string refused = R"({"error":"Sign in first","code":"signin_required"})";
    Logic::ErrorCode(refused.data(), refused.size(), code, sizeof(code));
    CHECK(std::strcmp(code, "signin_required") == 0);
    const std::string other = R"({"commands":[],"poll":15})";
    Logic::ErrorCode(other.data(), other.size(), code, sizeof(code));
    CHECK(code[0] == 0);
    Logic::ErrorCode("<html>", 6, code, sizeof(code));
    CHECK(code[0] == 0);

    CHECK(Logic::Classify(200, "") == Logic::Verdict::Ok);
    CHECK(Logic::Classify(401, "signin_required") == Logic::Verdict::SignedOut);
    CHECK(Logic::Classify(403, "banned") == Logic::Verdict::SignedOut);
    CHECK(Logic::Classify(401, "") == Logic::Verdict::Retry);  // a 401 without the code: a fault, not a refusal
    CHECK(Logic::Classify(403, "forbidden") == Logic::Verdict::Retry);
    CHECK(Logic::Classify(429, "rate_limited") == Logic::Verdict::Retry);
    CHECK(Logic::Classify(503, "unavailable") == Logic::Verdict::Retry);
    CHECK(Logic::Classify(0, nullptr) == Logic::Verdict::Retry);
}

void PlanTests()
{
    Logic::Inbox inbox;
    CHECK(Logic::PlanCommands(inbox).handOver == -1);
    CHECK(Parse(R"({"commands":[{"id":1,"action":"wipe"},{"id":2,"action":"install"},{"id":3,"action":"install_start"}]})",
                inbox));
    const Logic::Plan plan = Logic::PlanCommands(inbox);
    CHECK(plan.handOver == 1);
    CHECK(plan.fate[0] == Logic::Fate::Unsupported && plan.fate[1] == Logic::Fate::HandOver &&
          plan.fate[2] == Logic::Fate::Busy);
    CHECK(std::strcmp(Logic::RefusalText(Logic::Fate::Unsupported), "Unsupported command") == 0);
    CHECK(std::strcmp(Logic::RefusalText(Logic::Fate::Busy), "Device busy") == 0);
    CHECK(Parse(R"({"commands":[{"id":1,"action":"wipe"}]})", inbox));
    CHECK(Logic::PlanCommands(inbox).handOver == -1);
}

void ReportTests()
{
    char body[192];
    CHECK(Logic::ReportBody("handover", nullptr, body, sizeof(body)) == std::strlen(R"({"status":"handover"})"));
    CHECK(std::strcmp(body, R"({"status":"handover"})") == 0);
    Logic::ReportBody("failed", Logic::kRefusedByFirmware, body, sizeof(body));
    CHECK(std::strcmp(body, R"({"status":"failed","error":"Refused by the running firmware"})") == 0);
    Logic::ReportBody("failed", "a \"b\" \\ c\n\xE9", body, sizeof(body));
    CHECK(std::strcmp(body, R"({"status":"failed","error":"a \"b\" \\ c"})") == 0);
    Logic::ReportBody("failed", std::string(300, 'x').c_str(), body, sizeof(body));
    CHECK(std::strlen(body) == std::strlen(R"({"status":"failed","error":""})") + 120);
    char small[10];
    CHECK(Logic::ReportBody("handover", nullptr, small, sizeof(small)) == 0 && small[0] == 0);
}

void BackoffTests()
{
    Logic::Backoff backoff;
    const uint32_t expected[] = {5, 10, 20, 40, 80, 160, 300, 300, 300};
    for (uint32_t wait : expected) CHECK(backoff.Next() == wait);
    backoff.Reset();
    CHECK(backoff.Next() == 5);

    CHECK(Logic::PollSeconds(15) == 15 && Logic::PollSeconds(0) == 15);
    CHECK(Logic::PollSeconds(2) == 5 && Logic::PollSeconds(301) == 300);
}

void AudioTests()
{
    using Logic::Audio;
    CHECK(Logic::ClassifyAudio(0, 0) == Audio::Quiet);              // TX off
    CHECK(Logic::ClassifyAudio(2580, 2580) == Audio::Quiet);        // on, nobody writing
    CHECK(Logic::ClassifyAudio(2580, 2000) == Audio::Quiet);        // a short sound in 15 s
    CHECK(Logic::ClassifyAudio(2580, 0) == Audio::Streaming);       // a synth rendering all the time
    CHECK(Logic::ClassifyAudio(2580, 3) == Audio::Struggling);      // with underruns
    CHECK(Logic::ClassifyAudio(4, 2) == Audio::Struggling);
    CHECK(Logic::ClassifyAudio(4, 3) == Audio::Quiet);
    CHECK(Logic::ClassifyAudio(10, 50) == Audio::Quiet);            // counters read across a wrap
    CHECK(Logic::ClassifyAudio(0xFFFFFFFFu, 0xC0000000u) == Audio::Quiet);  // no overflow

    Logic::AudioPause pause;
    CHECK(pause.seconds == 0);
    pause.Underrun();
    CHECK(pause.seconds == 60);
    pause.Underrun();
    CHECK(pause.seconds == 120);
    pause.Underrun();
    CHECK(pause.seconds == 240);
    pause.Underrun();
    CHECK(pause.seconds == 300);
    pause.Underrun();
    CHECK(pause.seconds == 300);
    pause.Clean();
    CHECK(pause.seconds == 0);
}

void MemoryTests()
{
    CHECK(Logic::MemoryAllows(64 * 1024, 32 * 1024, true));
    CHECK(!Logic::MemoryAllows(39 * 1024, 32 * 1024, true));
    CHECK(!Logic::MemoryAllows(64 * 1024, 11 * 1024, true));
    CHECK(!Logic::MemoryAllows(64 * 1024, 32 * 1024, false));
    CHECK(Logic::MemoryAllows(96 * 1024, 30 * 1024, false));
    CHECK(!Logic::MemoryAllows(96 * 1024, 20 * 1024, false));
}

void PendingTests()
{
    // The Launcher reads these names (203-launcher docs/SHARED_SETTINGS.md); NVS keys have at most 15
    // characters.
    namespace Pending = Logic::Pending;
    CHECK(std::strcmp(Pending::kNamespace, "l_pending") == 0);
    for (const char* key : {Pending::kNamespace, Pending::kVersionKey, Pending::kCommandKey, Pending::kActionKey,
                            Pending::kUnlockKey, Pending::kApiKey})
        CHECK(std::strlen(key) > 0 && std::strlen(key) <= 15);
    CHECK(Pending::kVersion == 1);
}

void TicketTests()
{
    // The address the Launcher's bootloader reads: ESP-IDF's rtc_retain_mem_t (partition 8 bytes, reboot
    // counter 2, flags 1, reserve 1, custom 8, crc 4 = 24) at the top of RTC fast memory.
    CHECK(Ticket::kAddress == 0x600FFFF4u);
    CHECK(Ticket::kRetainMemBytes == 8 + 2 + 1 + 1 + 8 + 4);
    CHECK(Ticket::kRestart == 0x32303352u);  // "203R"
    CHECK(Ticket::Valid(Ticket::kRestart, Ticket::Inverse(Ticket::kRestart), Ticket::kRestart));
    CHECK(!Ticket::Valid(Ticket::kRestart, Ticket::kRestart, Ticket::kRestart));
    CHECK(!Ticket::Valid(0x32303342u, ~0x32303342u, Ticket::kRestart));  // the boot ticket is another
}

}  // namespace

int main()
{
    AllowedApiTests();
    UrlTests();
    InboxTests();
    ErrorTests();
    PlanTests();
    ReportTests();
    BackoffTests();
    AudioTests();
    MemoryTests();
    PendingTests();
    TicketTests();
    std::cout << "presence tests passed\n";
    return 0;
}
