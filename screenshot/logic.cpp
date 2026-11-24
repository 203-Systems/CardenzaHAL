// The screenshot's decisions (screenshot/logic.h). Pure: built into the HAL, the host tests and the 203 Launcher.
#include "screenshot/logic.h"

#include <cstdio>
#include <cstring>

namespace CardenzaHAL::Screenshot::Logic {

// ---- HOME + space ----------------------------------------------------------------------------------------

namespace {

// a is at or after b on a clock that wraps (milliseconds since the start, 32 bits).
bool AtOrAfter(uint32_t a, uint32_t b)
{
    return static_cast<int32_t>(a - b) >= 0;
}

}  // namespace

void HomeGesture::Reset(bool homeDown, bool spaceDown, uint32_t nowMs)
{
    *this = HomeGesture{};
    armed_ = !homeDown;
    home_ = homeDown;
    space_ = spaceDown;
    homeDownAt_ = pressStart_ = nowMs;
}

GestureEvents HomeGesture::Update(bool homeDown, bool spaceDown, bool spacePressed, uint32_t nowMs)
{
    GestureEvents events;
    if (homeUpValid_ && nowMs - homeUpAt_ >= kComboWindowMs) homeUpValid_ = false;
    if (spaceUpValid_ && nowMs - spaceUpAt_ >= kComboWindowMs) spaceUpValid_ = false;
    const bool homeWentDown = homeDown && !home_;
    const bool homeWentUp = !homeDown && home_;
    // A press of space: seen going down, or claimed by the keyboard between two looks (up again already).
    const bool spaceWentDown = (spaceDown && !space_) || (spacePressed && !space_);
    const bool spaceWentUp = (!spaceDown && space_) || (spacePressed && !spaceDown);

    if (homeWentDown && armed_)
    {
        if (menuAt_ != 0)
            menuAt_ = 0;  // pressed again while the menu waited: the same press going on
        else
        {
            pressShot_ = false;
            pressStart_ = nowMs;
            events.homeDown = true;
        }
        homeDownAt_ = nowMs;
        homeUpValid_ = false;
        // HOME while space is down, or a moment after it was let go.
        if (space_ || spaceUpValid_) events.screenshot = true;
    }
    if (spaceWentDown && armed_ && (homeDown || homeUpValid_)) events.screenshot = true;
    if (spaceWentUp)
    {
        spaceUpAt_ = nowMs;
        spaceUpValid_ = true;
    }
    if (homeWentUp)
    {
        if (!armed_)
            armed_ = true;  // held since watching began: it counts from the next press
        else
        {
            homeUpAt_ = nowMs;
            homeUpValid_ = true;
            const uint32_t held = nowMs - pressStart_;
            if (!pressShot_ && !events.screenshot && held >= kPressMs && held < kShortPressMs)
            {
                menuAt_ = nowMs + kComboWindowMs;
                if (menuAt_ == 0) menuAt_ = 1;
            }
        }
    }
    if (events.screenshot)
    {
        pressShot_ = true;
        menuAt_ = 0;
    }
    if (menuAt_ != 0 && !homeDown && AtOrAfter(nowMs, menuAt_))
    {
        menuAt_ = 0;
        events.openMenu = true;
    }
    home_ = homeDown;
    space_ = spaceDown;
    return events;
}

bool HomeGesture::SpaceIsSystems(const Window& window, bool homeDownNow, uint32_t nowMs)
{
    if (!window.armed) return false;
    return homeDownNow || window.home || (window.upValid && nowMs - window.upAt < kComboWindowMs);
}

bool SpaceGate::Update(bool spaceDown, bool systemHas, bool wait, uint32_t nowMs)
{
    switch (state_)
    {
        case State::Up:
            if (!spaceDown) return false;
            if (systemHas)
            {
                state_ = State::Claimed;
                claimed_ = true;
                return false;
            }
            if (wait)
            {
                state_ = State::Waiting;
                since_ = nowMs;
                released_ = false;
                return false;
            }
            state_ = State::Passed;
            return true;
        case State::Passed:
            if (spaceDown) return true;
            state_ = State::Up;
            return false;
        case State::Claimed:
            if (!spaceDown) state_ = State::Up;
            return false;
        case State::Waiting:
            if (!spaceDown) released_ = true;
            if (systemHas)
            {
                claimed_ = true;
                state_ = spaceDown ? State::Claimed : State::Up;
                return false;
            }
            if (nowMs - since_ < kComboWindowMs) return false;
            if (released_)
            {
                state_ = State::Tap;
                since_ = nowMs;
                return true;
            }
            state_ = State::Passed;
            return true;
        case State::Tap:
            if (nowMs - since_ < kTapMs) return true;
            state_ = State::Up;
            return false;
    }
    return false;
}

bool SpaceGate::TakeClaimed()
{
    const bool claimed = claimed_;
    claimed_ = false;
    return claimed;
}

// ---- Names -----------------------------------------------------------------------------------------------

void FirmwareTag(const char* projectName, char* out, std::size_t capacity)
{
    if (capacity == 0) return;
    std::size_t length = 0;
    const std::size_t most = capacity - 1 < kTagMax ? capacity - 1 : kTagMax;
    if (projectName != nullptr)
        for (const char* c = projectName; *c != 0 && length < most; ++c)
        {
            const char ch = *c;
            const bool keep = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
            out[length++] = keep ? ch : '-';
        }
    out[length] = 0;
    bool any = false;
    for (std::size_t i = 0; i < length; ++i) any = any || out[i] != '-';
    if (!any) std::snprintf(out, capacity, "%s", "Firmware");
}

void Stamp(int64_t unixSeconds, char* out, std::size_t capacity)
{
    if (unixSeconds < 0) unixSeconds = 0;
    const int64_t days = unixSeconds / 86400;
    const int64_t secondOfDay = unixSeconds % 86400;
    // days since 1970-01-01 to a civil date (H. Hinnant's civil_from_days).
    const int64_t z = days + 719468;
    const int64_t era = z / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t day = doy - (153 * mp + 2) / 5 + 1;
    const int64_t month = mp < 10 ? mp + 3 : mp - 9;
    const int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    std::snprintf(out, capacity, "%04d-%02d-%02d_%02d%02d%02d", static_cast<int>(year), static_cast<int>(month),
                  static_cast<int>(day), static_cast<int>(secondOfDay / 3600), static_cast<int>(secondOfDay / 60 % 60),
                  static_cast<int>(secondOfDay % 60));
}

bool DatedName(int64_t unixSeconds, int sequence, const char* tag, char* out, std::size_t capacity)
{
    char stamp[24];
    Stamp(unixSeconds, stamp, sizeof(stamp));
    const int written = sequence <= 1 ? std::snprintf(out, capacity, "%s_%s.png", stamp, tag)
                                      : std::snprintf(out, capacity, "%s-%d_%s.png", stamp, sequence, tag);
    return written > 0 && static_cast<std::size_t>(written) < capacity && ValidName(out);
}

bool NumberedName(uint32_t number, const char* tag, char* out, std::size_t capacity)
{
    const int written = std::snprintf(out, capacity, "shot-%04u_%s.png", static_cast<unsigned>(number), tag);
    return written > 0 && static_cast<std::size_t>(written) < capacity && ValidName(out);
}

uint32_t ShotNumber(const char* name)
{
    if (name == nullptr || std::strncmp(name, "shot-", 5) != 0) return 0;
    uint32_t number = 0;
    const char* c = name + 5;
    if (*c < '0' || *c > '9') return 0;
    for (; *c >= '0' && *c <= '9'; ++c)
    {
        if (number > 100000000u) return 0;
        number = number * 10 + static_cast<uint32_t>(*c - '0');
    }
    return *c == '_' ? number : 0;
}

bool ValidName(const char* name)
{
    if (name == nullptr) return false;
    const std::size_t length = std::strlen(name);
    if (length < 5 || length > kNameMax || name[0] == '.') return false;
    for (std::size_t i = 0; i < length; ++i)
    {
        const char ch = name[i];
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' ||
                        ch == '_' || ch == '.';
        if (!ok) return false;
    }
    return std::strcmp(name + length - 4, ".png") == 0;
}

bool TempName(const char* name, char* out, std::size_t capacity)
{
    const int written = std::snprintf(out, capacity, ".%s.tmp", name);
    return written > 0 && static_cast<std::size_t>(written) < capacity;
}

// ---- The upload queue -------------------------------------------------------------------------------------

namespace Queue {

namespace {

// The journal's complete lines, one at a time: kind and name (pointing into the text).
struct Lines {
    const char* text;
    std::size_t length;
    std::size_t at = 0;
    bool Next(char& kind, const char*& name, std::size_t& nameLength)
    {
        while (at < length)
        {
            const char* line = text + at;
            const void* end = std::memchr(line, '\n', length - at);
            if (end == nullptr) return false;  // a torn last line
            const std::size_t lineLength = static_cast<const char*>(end) - line;
            at += lineLength + 1;
            std::size_t n = lineLength;
            if (n > 0 && line[n - 1] == '\r') --n;
            if (n < 3 || line[1] != ' ') continue;
            if (line[0] != kQueued && line[0] != kUploaded && line[0] != kSkipped) continue;
            kind = line[0];
            name = line + 2;
            nameLength = n - 2;
            return true;
        }
        return false;
    }
};

bool Same(const char* a, std::size_t aLength, const char* b, std::size_t bLength)
{
    return aLength == bLength && std::memcmp(a, b, aLength) == 0;
}

// The Q line whose name starts at `name` is still pending: no U or S of that name after `from`.
bool StillPending(const char* text, std::size_t length, std::size_t from, const char* name, std::size_t nameLength)
{
    Lines later{text, length, from};
    char kind;
    const char* other;
    std::size_t otherLength;
    while (later.Next(kind, other, otherLength))
        if (kind != kQueued && Same(name, nameLength, other, otherLength)) return false;
    return true;
}

// No later Q of the same name (a name queued twice counts once, at its last Q).
bool LastQueued(const char* text, std::size_t length, std::size_t from, const char* name, std::size_t nameLength)
{
    Lines later{text, length, from};
    char kind;
    const char* other;
    std::size_t otherLength;
    while (later.Next(kind, other, otherLength))
        if (kind == kQueued && Same(name, nameLength, other, otherLength)) return false;
    return true;
}

}  // namespace

std::size_t Line(char kind, const char* name, char* out, std::size_t capacity)
{
    if ((kind != kQueued && kind != kUploaded && kind != kSkipped) || !ValidName(name)) return 0;
    const int written = std::snprintf(out, capacity, "%c %s\n", kind, name);
    return written > 0 && static_cast<std::size_t>(written) < capacity ? static_cast<std::size_t>(written) : 0;
}

bool FirstPending(const char* text, std::size_t length, char* out, std::size_t capacity, std::size_t* pending)
{
    Lines lines{text, length};
    char kind;
    const char* name;
    std::size_t nameLength;
    bool found = false;
    std::size_t count = 0;
    while (lines.Next(kind, name, nameLength))
    {
        if (kind != kQueued || !StillPending(text, length, lines.at, name, nameLength)) continue;
        if (!LastQueued(text, length, lines.at, name, nameLength)) continue;
        ++count;
        if (!found && nameLength < capacity)
        {
            std::memcpy(out, name, nameLength);
            out[nameLength] = 0;
            found = true;
        }
    }
    if (pending != nullptr) *pending = count;
    return found;
}

std::size_t Compact(const char* text, std::size_t length, char* out, std::size_t capacity)
{
    Lines lines{text, length};
    char kind;
    const char* name;
    std::size_t nameLength;
    std::size_t used = 0;
    while (lines.Next(kind, name, nameLength))
    {
        if (kind != kQueued || !StillPending(text, length, lines.at, name, nameLength)) continue;
        if (!LastQueued(text, length, lines.at, name, nameLength)) continue;
        if (used + nameLength + 3 > capacity) break;
        out[used++] = kQueued;
        out[used++] = ' ';
        std::memcpy(out + used, name, nameLength);
        used += nameLength;
        out[used++] = '\n';
    }
    return used;
}

}  // namespace Queue

// ---- Upload answers ----------------------------------------------------------------------------------------

Upload Classify(int httpStatus, const char* errorCode)
{
    const bool code = errorCode != nullptr && errorCode[0] != 0;
    if (httpStatus == 200 || httpStatus == 201) return Upload::Done;
    if (httpStatus == 400 || httpStatus == 413 || httpStatus == 415 || httpStatus == 422) return Upload::Skip;
    if (httpStatus == 401) return Upload::SignedOut;
    if (httpStatus == 403) return code && std::strcmp(errorCode, "banned") == 0 ? Upload::SignedOut : Upload::StopRun;
    if (httpStatus == 404 || httpStatus == 405 || httpStatus == 409 || httpStatus == 410) return Upload::StopRun;
    return Upload::Later;
}

// ---- The effect ------------------------------------------------------------------------------------------

namespace Effect {

int Bayer(int x, int y)
{
    static constexpr uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    return kBayer[y & 3][x & 3];
}

bool Flash(uint32_t t, int x, int y)
{
    if (t >= kFlashMs) return false;
    const int cover = static_cast<int>(16 * (kFlashMs - t) / kFlashMs);
    return Bayer(x, y) < cover;
}

bool Card(uint32_t t, int x, int y)
{
    if (!kCard.Contains(x, y) || t < kCardInAtMs) return false;
    if (t < kCardInAtMs + kDissolveMs) return Bayer(x, y) < 1 + static_cast<int>(15 * (t - kCardInAtMs) / kDissolveMs);
    if (t < kCardOutAtMs) return true;
    if (t >= kCardOutAtMs + kDissolveMs) return false;
    return Bayer(x, y) < 16 - static_cast<int>(16 * (t - kCardOutAtMs) / kDissolveMs);
}

}  // namespace Effect

// ---- Over USB --------------------------------------------------------------------------------------------

namespace Usb {

std::size_t Header(Format format, Saved saved, int width, int height, const char* name, uint32_t dataBytes, uint8_t* out,
                   std::size_t capacity)
{
    const std::size_t nameLength = name != nullptr ? std::strlen(name) : 0;
    const std::size_t length = 8 + 1 + nameLength + 4;
    if (nameLength > kNameMax || capacity < length) return 0;
    out[0] = 'K';
    out[1] = 1;
    out[2] = static_cast<uint8_t>(format);
    out[3] = static_cast<uint8_t>(saved);
    out[4] = static_cast<uint8_t>(width);
    out[5] = static_cast<uint8_t>(width >> 8);
    out[6] = static_cast<uint8_t>(height);
    out[7] = static_cast<uint8_t>(height >> 8);
    out[8] = static_cast<uint8_t>(nameLength);
    if (nameLength != 0) std::memcpy(out + 9, name, nameLength);
    uint8_t* tail = out + 9 + nameLength;
    tail[0] = static_cast<uint8_t>(dataBytes);
    tail[1] = static_cast<uint8_t>(dataBytes >> 8);
    tail[2] = static_cast<uint8_t>(dataBytes >> 16);
    tail[3] = static_cast<uint8_t>(dataBytes >> 24);
    return length;
}

}  // namespace Usb

}  // namespace CardenzaHAL::Screenshot::Logic
