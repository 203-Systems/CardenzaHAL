// The presence service's decisions (net/presence_logic.h). Pure C++: the host tests compile it too.
#include "net/presence_logic.h"

#include <cstdio>
#include <cstring>

#include "net/http_request.h"
#include "net/launcher_settings.h"

namespace CardenzaHAL::Presence::Logic {

namespace {

char Lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool SameHost(const char* host, std::size_t length, const char* allowed)
{
    if (std::strlen(allowed) != length) return false;
    for (std::size_t i = 0; i < length; ++i)
        if (Lower(host[i]) != allowed[i]) return false;
    return true;
}

// An IPv4 address in dotted decimal, each part 0..255 without leading zeros.
bool Ipv4(const char* host, std::size_t length, uint8_t out[4])
{
    std::size_t at = 0;
    for (int part = 0; part < 4; ++part)
    {
        if (part > 0)
        {
            if (at >= length || host[at] != '.') return false;
            ++at;
        }
        const std::size_t start = at;
        unsigned value = 0;
        while (at < length && host[at] >= '0' && host[at] <= '9' && at - start < 3)
            value = value * 10 + static_cast<unsigned>(host[at++] - '0');
        if (at == start || value > 255 || (at - start > 1 && host[start] == '0')) return false;
        out[part] = static_cast<uint8_t>(value);
    }
    return at == length;
}

bool LocalAddress(const uint8_t a[4])
{
    return a[0] == 127 || a[0] == 10 || (a[0] == 172 && a[1] >= 16 && a[1] <= 31) || (a[0] == 192 && a[1] == 168);
}

// --- A small JSON reader: enough for the inbox, skips what it does not need. -----------------------------

class Reader {
public:
    Reader(const char* text, std::size_t length) : at_(text), end_(text + length) {}

    void Space()
    {
        while (at_ < end_ && (*at_ == ' ' || *at_ == '\t' || *at_ == '\n' || *at_ == '\r')) ++at_;
    }
    bool Take(char c)
    {
        Space();
        if (at_ < end_ && *at_ == c)
        {
            ++at_;
            return true;
        }
        return false;
    }
    char Peek()
    {
        Space();
        return at_ < end_ ? *at_ : 0;
    }
    bool AtEnd()
    {
        Space();
        return at_ == end_;
    }

    // A string into `out` (UTF-8, zero-terminated, cut at a character when it does not fit; `out` may be
    // null to skip it). `fits`: it was not cut.
    bool String(char* out, std::size_t capacity, bool* fits = nullptr)
    {
        if (!Take('"')) return false;
        std::size_t written = 0;
        bool whole = true;
        auto put = [&](const char* bytes, std::size_t count) {
            if (out == nullptr) return;
            if (!whole || written + count >= capacity)
            {
                whole = false;
                return;
            }
            std::memcpy(out + written, bytes, count);
            written += count;
        };
        while (true)
        {
            if (at_ >= end_) return false;
            const unsigned char c = static_cast<unsigned char>(*at_++);
            if (c == '"') break;
            if (c < 0x20) return false;
            if (c != '\\')
            {
                const char byte = static_cast<char>(c);
                // A multi-byte character goes in whole or not at all.
                if (c >= 0x80 && (c < 0xC2 || c > 0xF4)) return false;  // not a lead byte
                std::size_t count = c < 0x80 ? 1 : (c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : 2));
                if (count > 1)
                {
                    if (static_cast<std::size_t>(end_ - at_) < count - 1) return false;
                    for (std::size_t i = 0; i < count - 1; ++i)
                        if ((static_cast<unsigned char>(at_[i]) & 0xC0) != 0x80) return false;
                    put(at_ - 1, count);
                    at_ += count - 1;
                }
                else
                {
                    put(&byte, 1);
                }
                continue;
            }
            if (at_ >= end_) return false;
            const char escape = *at_++;
            char simple = 0;
            switch (escape)
            {
            case '"': simple = '"'; break;
            case '\\': simple = '\\'; break;
            case '/': simple = '/'; break;
            case 'b': simple = '\b'; break;
            case 'f': simple = '\f'; break;
            case 'n': simple = '\n'; break;
            case 'r': simple = '\r'; break;
            case 't': simple = '\t'; break;
            case 'u': break;
            default: return false;
            }
            if (escape != 'u')
            {
                put(&simple, 1);
                continue;
            }
            uint32_t code = 0;
            if (!Hex4(code)) return false;
            if (code >= 0xD800 && code <= 0xDBFF)
            {
                uint32_t low = 0;
                if (end_ - at_ < 2 || at_[0] != '\\' || at_[1] != 'u') return false;
                at_ += 2;
                if (!Hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            }
            else if (code >= 0xDC00 && code <= 0xDFFF)
            {
                return false;
            }
            char bytes[4];
            std::size_t count;
            if (code < 0x80)
            {
                bytes[0] = static_cast<char>(code);
                count = 1;
            }
            else if (code < 0x800)
            {
                bytes[0] = static_cast<char>(0xC0 | (code >> 6));
                bytes[1] = static_cast<char>(0x80 | (code & 0x3F));
                count = 2;
            }
            else if (code < 0x10000)
            {
                bytes[0] = static_cast<char>(0xE0 | (code >> 12));
                bytes[1] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                bytes[2] = static_cast<char>(0x80 | (code & 0x3F));
                count = 3;
            }
            else
            {
                bytes[0] = static_cast<char>(0xF0 | (code >> 18));
                bytes[1] = static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                bytes[2] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                bytes[3] = static_cast<char>(0x80 | (code & 0x3F));
                count = 4;
            }
            put(bytes, count);
        }
        if (out != nullptr && capacity > 0) out[written] = 0;
        if (fits != nullptr) *fits = whole;
        return true;
    }

    // A number. `digits`: its text when it is a whole number of at most 19 digits, else empty.
    bool Number(char* digits, std::size_t capacity)
    {
        Space();
        const char* start = at_;
        if (at_ < end_ && *at_ == '-') ++at_;
        const char* intStart = at_;
        while (at_ < end_ && *at_ >= '0' && *at_ <= '9') ++at_;
        const std::size_t intLength = static_cast<std::size_t>(at_ - intStart);
        if (intLength == 0 || (intLength > 1 && *intStart == '0')) return false;
        bool whole = start == intStart;
        if (at_ < end_ && *at_ == '.')
        {
            ++at_;
            const char* fraction = at_;
            while (at_ < end_ && *at_ >= '0' && *at_ <= '9') ++at_;
            if (at_ == fraction) return false;
            whole = false;
        }
        if (at_ < end_ && (*at_ == 'e' || *at_ == 'E'))
        {
            ++at_;
            if (at_ < end_ && (*at_ == '+' || *at_ == '-')) ++at_;
            const char* exponent = at_;
            while (at_ < end_ && *at_ >= '0' && *at_ <= '9') ++at_;
            if (at_ == exponent) return false;
            whole = false;
        }
        if (digits != nullptr && capacity > 0)
        {
            digits[0] = 0;
            if (whole && intLength <= 19 && intLength < capacity)
            {
                std::memcpy(digits, intStart, intLength);
                digits[intLength] = 0;
            }
        }
        return true;
    }

    // Any value, skipped.
    bool Skip(int depth = 0)
    {
        if (depth > 32) return false;
        switch (Peek())
        {
        case '"': return String(nullptr, 0);
        case '{': {
            Take('{');
            if (Take('}')) return true;
            do
            {
                if (!String(nullptr, 0) || !Take(':') || !Skip(depth + 1)) return false;
            } while (Take(','));
            return Take('}');
        }
        case '[': {
            Take('[');
            if (Take(']')) return true;
            do
            {
                if (!Skip(depth + 1)) return false;
            } while (Take(','));
            return Take(']');
        }
        case 't': return Word("true");
        case 'f': return Word("false");
        case 'n': return Word("null");
        default: return Number(nullptr, 0);
        }
    }

    bool Null()
    {
        if (Peek() != 'n') return false;
        return Word("null");
    }

private:
    bool Hex4(uint32_t& value)
    {
        if (end_ - at_ < 4) return false;
        value = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char c = *at_++;
            value <<= 4;
            if (c >= '0' && c <= '9')
                value |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                value |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                value |= static_cast<uint32_t>(c - 'A' + 10);
            else
                return false;
        }
        return true;
    }
    bool Word(const char* word)
    {
        Space();
        const std::size_t length = std::strlen(word);
        if (static_cast<std::size_t>(end_ - at_) < length || std::memcmp(at_, word, length) != 0) return false;
        at_ += length;
        return true;
    }

    const char* at_;
    const char* end_;
};

// Keys are short: one that does not fit is no key we know.
constexpr std::size_t kKeyMax = 16;

// {"key": value, ...}, calling `field(key)` for each; `field` reads or skips the value.
template <typename Field>
bool Object(Reader& reader, Field&& field)
{
    if (!reader.Take('{')) return false;
    if (reader.Take('}')) return true;
    do
    {
        char key[kKeyMax + 1];
        bool fits = false;
        if (!reader.String(key, sizeof(key), &fits) || !reader.Take(':')) return false;
        if (!fits) key[0] = 0;
        if (!field(key)) return false;
    } while (reader.Take(','));
    return reader.Take('}');
}

bool IdText(const char* text)
{
    const std::size_t length = std::strlen(text);
    if (length == 0 || length > kIdMax) return false;
    for (std::size_t i = 0; i < length; ++i)
    {
        const char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_'))
            return false;
    }
    return true;
}

bool UnlockToken(const char* text)
{
    if (std::strlen(text) != kUnlockLength || std::strncmp(text, "cfu_", 4) != 0) return false;
    for (std::size_t i = 4; i < kUnlockLength; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
    return true;
}

// Text for a log line or the firmware's screen: control characters become spaces, cut at a character.
bool Label(Reader& reader, char* out, std::size_t capacity)
{
    if (reader.Null())
    {
        out[0] = 0;
        return true;
    }
    if (reader.Peek() != '"') return reader.Skip();
    char text[160];
    if (!reader.String(text, sizeof(text))) return false;
    for (char* c = text; *c != 0; ++c)
        if (static_cast<unsigned char>(*c) < 0x20 || *c == 0x7F) *c = ' ';
    Net::CopyUtf8(text, out, capacity);
    return true;
}

bool ReadCommand(Reader& reader, Command& command, bool& usable)
{
    usable = false;
    bool haveId = false;
    char action[24] = {};
    const bool ok = Object(reader, [&](const char* key) {
        if (std::strcmp(key, "id") == 0)
        {
            if (reader.Peek() == '"')
            {
                char text[kIdMax + 2];
                bool fits = false;
                if (!reader.String(text, sizeof(text), &fits)) return false;
                if (fits && IdText(text))
                {
                    std::strcpy(command.id, text);
                    haveId = true;
                }
                return true;
            }
            if (reader.Peek() == '-' || (reader.Peek() >= '0' && reader.Peek() <= '9'))
            {
                char digits[24];
                if (!reader.Number(digits, sizeof(digits))) return false;
                if (digits[0] != 0)
                {
                    std::strcpy(command.id, digits);
                    haveId = true;
                }
                return true;
            }
            return reader.Skip();
        }
        if (std::strcmp(key, "action") == 0)
        {
            if (reader.Peek() != '"') return reader.Skip();
            bool fits = false;
            if (!reader.String(action, sizeof(action), &fits)) return false;
            if (!fits) action[0] = 0;
            return true;
        }
        if (std::strcmp(key, "unlock") == 0)
        {
            if (reader.Peek() != '"') return reader.Skip();
            char text[kUnlockLength + 2];
            bool fits = false;
            if (!reader.String(text, sizeof(text), &fits)) return false;
            if (fits && UnlockToken(text)) std::strcpy(command.unlock, text);
            return true;
        }
        if (std::strcmp(key, "firmware") == 0 && reader.Peek() == '{')
            return Object(reader, [&](const char* inner) {
                return std::strcmp(inner, "name") == 0 ? Label(reader, command.firmwareName, sizeof(command.firmwareName))
                                                        : reader.Skip();
            });
        if (std::strcmp(key, "version") == 0 && reader.Peek() == '{')
            return Object(reader, [&](const char* inner) {
                return std::strcmp(inner, "version") == 0 ? Label(reader, command.version, sizeof(command.version))
                                                           : reader.Skip();
            });
        return reader.Skip();
    });
    if (!ok) return false;
    command.action = ActionOf(action);
    usable = haveId;
    return true;
}

void Append(char* out, std::size_t capacity, std::size_t& length, const char* text, bool& fits)
{
    const std::size_t add = std::strlen(text);
    if (!fits || length + add >= capacity)
    {
        fits = false;
        return;
    }
    std::memcpy(out + length, text, add);
    length += add;
    out[length] = 0;
}

// api without a trailing '/'.
std::size_t ApiLength(const char* api)
{
    std::size_t length = std::strlen(api);
    while (length > 0 && api[length - 1] == '/') --length;
    return length;
}

}  // namespace

Action ActionOf(const char* text)
{
    if (text == nullptr) return Action::Unknown;
    if (std::strcmp(text, "install") == 0) return Action::Install;
    if (std::strcmp(text, "install_start") == 0) return Action::InstallStart;
    if (std::strcmp(text, "update_launcher") == 0) return Action::UpdateLauncher;
    return Action::Unknown;
}

const char* ActionText(Action action)
{
    switch (action)
    {
    case Action::Install: return "install";
    case Action::InstallStart: return "install_start";
    case Action::UpdateLauncher: return "update_launcher";
    case Action::Unknown: break;
    }
    return "";
}

bool AllowedApi(const char* api, bool localHttp)
{
    if (api == nullptr) return false;
    Net::UrlParts parts;
    if (!Net::ParseUrl(api, parts)) return false;
    // The API sits below a path ("/api", "/community/api"): a bare host is not it.
    if (parts.pathLength < 2 || std::strchr(api, '?') != nullptr) return false;
    if (parts.https)
    {
        if (parts.port != 443) return false;
        for (const char* host : kAllowedHosts)
            if (SameHost(parts.host, parts.hostLength, host)) return true;
        return false;
    }
    uint8_t address[4];
    return localHttp && Ipv4(parts.host, parts.hostLength, address) && LocalAddress(address);
}

bool InboxUrl(const char* api, char* out, std::size_t capacity)
{
    const int written = std::snprintf(out, capacity, "%.*s/launcher/inbox?from=firmware",
                                      static_cast<int>(ApiLength(api)), api);
    return written > 0 && static_cast<std::size_t>(written) < capacity;
}

bool CommandUrl(const char* api, const char* id, char* out, std::size_t capacity)
{
    if (!IdText(id)) return false;
    const int written = std::snprintf(out, capacity, "%.*s/launcher/commands/%s", static_cast<int>(ApiLength(api)),
                                      api, id);
    return written > 0 && static_cast<std::size_t>(written) < capacity;
}

bool ParseInbox(const char* json, std::size_t length, Inbox& inbox)
{
    inbox = Inbox{};
    if (json == nullptr) return false;
    Reader reader(json, length);
    bool haveCommands = false;
    const bool ok = Object(reader, [&](const char* key) {
        if (std::strcmp(key, "commands") == 0)
        {
            if (reader.Peek() != '[') return false;
            haveCommands = true;
            reader.Take('[');
            if (reader.Take(']')) return true;
            do
            {
                if (reader.Peek() != '{')
                {
                    if (!reader.Skip()) return false;
                    ++inbox.dropped;
                    continue;
                }
                Command command;
                bool usable = false;
                if (!ReadCommand(reader, command, usable)) return false;
                if (usable && inbox.count < kMaxCommands)
                    inbox.commands[inbox.count++] = command;
                else
                    ++inbox.dropped;
            } while (reader.Take(','));
            return reader.Take(']');
        }
        if (std::strcmp(key, "poll") == 0)
        {
            if (reader.Peek() == '-' || (reader.Peek() >= '0' && reader.Peek() <= '9'))
            {
                char digits[24];
                if (!reader.Number(digits, sizeof(digits))) return false;
                uint32_t value = 0;
                bool small = digits[0] != 0 && digits[0] != '-' && std::strlen(digits) <= 6;
                for (const char* c = digits; small && *c != 0; ++c) value = value * 10 + static_cast<uint32_t>(*c - '0');
                if (small) inbox.pollSeconds = PollSeconds(value);
                return true;
            }
            return reader.Skip();
        }
        return reader.Skip();
    });
    if (!ok || !haveCommands || !reader.AtEnd())
    {
        inbox = Inbox{};
        return false;
    }
    return true;
}

void ErrorCode(const char* json, std::size_t length, char* code, std::size_t capacity)
{
    if (capacity == 0) return;
    code[0] = 0;
    if (json == nullptr) return;
    Reader reader(json, length);
    char found[40] = {};
    const bool ok = Object(reader, [&](const char* key) {
        if (std::strcmp(key, "code") == 0 && reader.Peek() == '"')
        {
            bool fits = false;
            if (!reader.String(found, sizeof(found), &fits)) return false;
            if (!fits) found[0] = 0;
            return true;
        }
        return reader.Skip();
    });
    if (ok) std::snprintf(code, capacity, "%s", found);
}

uint32_t PollSeconds(uint32_t answered)
{
    if (answered == 0) return kDefaultPollSeconds;
    return answered < 5 ? 5 : (answered > 300 ? 300 : answered);
}

Verdict Classify(int httpStatus, const char* errorCode)
{
    if (httpStatus == 200) return Verdict::Ok;
    const bool code = errorCode != nullptr;
    if (httpStatus == 401 && code && std::strcmp(errorCode, "signin_required") == 0) return Verdict::SignedOut;
    if (httpStatus == 403 && code && std::strcmp(errorCode, "banned") == 0) return Verdict::SignedOut;
    return Verdict::Retry;
}

Plan PlanCommands(const Inbox& inbox)
{
    Plan plan;
    for (std::size_t i = 0; i < inbox.count && i < kMaxCommands; ++i)
    {
        if (!inbox.commands[i].Known())
            plan.fate[i] = Fate::Unsupported;
        else if (plan.handOver < 0)
        {
            plan.fate[i] = Fate::HandOver;
            plan.handOver = static_cast<int>(i);
        }
        else
            plan.fate[i] = Fate::Busy;
    }
    return plan;
}

const char* RefusalText(Fate fate)
{
    switch (fate)
    {
    case Fate::Unsupported: return "Unsupported command";
    case Fate::Busy: return "Device busy";
    default: return "";
    }
}

std::size_t ReportBody(const char* status, const char* error, char* out, std::size_t capacity)
{
    if (capacity == 0) return 0;
    out[0] = 0;
    std::size_t length = 0;
    bool fits = true;
    Append(out, capacity, length, "{\"status\":\"", fits);
    Append(out, capacity, length, status, fits);
    Append(out, capacity, length, "\"", fits);
    if (error != nullptr && error[0] != 0)
    {
        Append(out, capacity, length, ",\"error\":\"", fits);
        std::size_t kept = 0;
        for (const char* c = error; *c != 0 && kept < 120; ++c)
        {
            if (*c < 0x20 || *c > 0x7E) continue;
            const char text[3] = {'\\', *c, 0};
            Append(out, capacity, length, *c == '"' || *c == '\\' ? text : text + 1, fits);
            ++kept;
        }
        Append(out, capacity, length, "\"", fits);
    }
    Append(out, capacity, length, "}", fits);
    if (!fits)
    {
        out[0] = 0;
        return 0;
    }
    return length;
}

uint32_t Backoff::Next()
{
    const uint32_t wait = failures >= 6 ? 300 : 5u << failures;
    if (failures < 6) ++failures;
    return wait;
}

Audio ClassifyAudio(uint32_t sentBlocks, uint32_t emptyBlocks)
{
    if (sentBlocks == 0) return Audio::Quiet;
    if (emptyBlocks > sentBlocks) emptyBlocks = sentBlocks;
    if (uint64_t{emptyBlocks} * 4 >= uint64_t{sentBlocks} * 3) return Audio::Quiet;
    return emptyBlocks == 0 ? Audio::Streaming : Audio::Struggling;
}

Decision Decide(const Situation& s)
{
    const auto decide = [](Step step, Wait reason) { return Decision{step, reason}; };
    if (s.activity == Activity::Quiet)
        return s.connected ? decide(Step::LetGo, Wait::Activity) : decide(Step::Wait, Wait::Activity);
    if (s.connected && s.freeInternal < s.floor) return decide(Step::LetGo, Wait::Ram);
    if (s.audio == Audio::Struggling) return decide(Step::Wait, Wait::AudioLate);
    const std::size_t need = !s.tlsInPsram ? kRequestBytesTlsInternal
                             : s.wifiInPsram ? kRequestBytesAllInPsram
                                             : kRequestBytesTlsInPsram;
    if (!s.connected)
    {
        if (s.activity == Activity::RealTime) return decide(Step::Wait, Wait::Activity);
        if (s.ramPaused || s.freeInternal < s.floor + s.wifiCost + need || s.largestInternal < kWifiLargestBlock)
            return decide(Step::Wait, Wait::Ram);
        return decide(Step::Join, Wait::None);
    }
    if (s.firmwareSending) return decide(Step::Wait, Wait::FirmwareRequest);
    const std::size_t block = s.tlsInPsram ? kRequestBlockTlsInPsram : kRequestBlockTlsInternal;
    if (s.freeInternal < s.floor + need || s.largestInternal < block) return decide(Step::Wait, Wait::Ram);
    return decide(Step::Ask, Wait::None);
}

std::size_t LearnWifiCost(std::size_t previous, std::size_t freeBefore, std::size_t freeAfter)
{
    if (freeAfter >= freeBefore) return previous;
    const std::size_t taken = freeBefore - freeAfter;
    if (taken < 16 * 1024) return 16 * 1024;
    return taken > 96 * 1024 ? 96 * 1024 : taken;
}

void HeaderText(const char* text, char* out, std::size_t capacity)
{
    if (capacity == 0) return;
    out[0] = 0;
    if (text == nullptr) return;
    while (*text == ' ') ++text;
    std::size_t length = 0;
    for (; *text != 0 && length < kHeaderTextMax && length + 1 < capacity; ++text)
    {
        const unsigned char c = static_cast<unsigned char>(*text);
        out[length++] = c >= 0x20 && c <= 0x7E && c != '"' && c != '\\' ? static_cast<char>(c) : '_';
    }
    while (length > 0 && out[length - 1] == ' ') --length;
    out[length] = 0;
}

bool AppendedDigestOffset(ReadAt read, void* context, uint32_t limit, uint32_t& offset)
{
    // esp_image_header_t (ESP-IDF bootloader_support/include/esp_app_format.h) and the segment headers.
    constexpr uint32_t kHeaderBytes = 24;
    constexpr uint32_t kSegmentHeaderBytes = 8;
    constexpr uint8_t kMagic = 0xE9;
    constexpr uint32_t kMaxSegments = 16;
    constexpr uint32_t kDigestBytes = 32;
    uint8_t header[kHeaderBytes];
    if (read == nullptr || limit < kHeaderBytes || !read(context, 0, header, kHeaderBytes)) return false;
    const uint32_t segments = header[1];
    if (header[0] != kMagic || segments == 0 || segments > kMaxSegments || header[23] != 1) return false;
    uint64_t at = kHeaderBytes;
    for (uint32_t i = 0; i < segments; ++i)
    {
        uint8_t segment[kSegmentHeaderBytes];
        if (at + kSegmentHeaderBytes > limit || !read(context, static_cast<uint32_t>(at), segment, kSegmentHeaderBytes))
            return false;
        const uint32_t length = static_cast<uint32_t>(segment[4]) | static_cast<uint32_t>(segment[5]) << 8 |
                                static_cast<uint32_t>(segment[6]) << 16 | static_cast<uint32_t>(segment[7]) << 24;
        at += kSegmentHeaderBytes + uint64_t{length};
        if (at > limit) return false;
    }
    const uint64_t digest = (at + 16) & ~uint64_t{15};
    if (digest + kDigestBytes > limit) return false;
    offset = static_cast<uint32_t>(digest);
    return true;
}

}  // namespace CardenzaHAL::Presence::Logic
