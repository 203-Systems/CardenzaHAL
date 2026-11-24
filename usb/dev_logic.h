#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// The development channel's pure parts (docs/usb-dev.md): message framing, 7-bit packing, the screen's
// run-length code, the JSON writer, card paths, and reading an application image's length and an NVS page's
// use. No ESP-IDF here: usb/dev.cpp uses them on the board, tests/usb_dev_test.cpp on the host.
namespace CardenzaHAL::USB::Dev {

enum Op : uint8_t {
    kHello = 0x00,
    kInfo = 0x01,
    kPartitions = 0x02,
    kMemory = 0x03,
    kStorage = 0x04,
    kStatus = 0x05,
    kI2c = 0x06,
    kArg = 0x0E,
    kScreen = 0x10,
    kKeys = 0x11,
    // A screenshot as HOME + space takes one (screenshot/screenshot.h): saved on the card and/or sent back.
    kScreenshot = 0x12,
    kCoredumpInfo = 0x20,
    kCoredumpRead = 0x21,
    kCoredumpErase = 0x22,
    kLs = 0x30,
    kGet = 0x31,
    kPutBegin = 0x32,
    kPutData = 0x33,
    kPutEnd = 0x34,
    kPutAbort = 0x35,
    kRm = 0x36,
    kMkdir = 0x37,
    kMv = 0x38,
    // The board's settings (system/settings.h): always there, also without CONFIG_CARDENZA_HAL_USB_DEV.
    kSettings = 0x40,
    kSettingGet = 0x41,
    kSettingSet = 0x42,
    kSettingsReset = 0x43,
    kCancel = 0x7F,
};

// 3: the settings (kSettings ...). 4: the serial port and WebUSB carry the channel too, INFO is answered also
// without the development channel, and says how the board shows up on USB. HELLO answers it.
constexpr int kProtocol = 4;

// A request's little-endian i32 (SETTING_SET's value).
inline int32_t ReadI32(const uint8_t* p)
{
    return static_cast<int32_t>(uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24);
}

constexpr uint8_t kFlagLast = 0x01;
constexpr uint8_t kFlagError = 0x02;
constexpr uint8_t kAnswerCommand = 0x45;          // USB::kCommandDev
constexpr std::size_t kReport = 63;               // USB::kReportSize
constexpr std::size_t kMaxArgs = 52;              // a request's args after unpacking
constexpr std::size_t kHidPayload = 57;           // [C5 id lo hi flags length] + payload = 63
constexpr std::size_t kSysexPayload = 48;         // packed: 55, + F0 24 45 id lo hi flags + F7 = 63
constexpr std::size_t kArgBuffer = 512;           // ARG's buffer
constexpr std::size_t kMaxPath = 255;
constexpr std::size_t kSysexMessage = 8 + kSysexPayload + (kSysexPayload + 6) / 7;

constexpr std::size_t PackedSize(std::size_t bytes) { return bytes + (bytes + 6) / 7; }

// Each group of up to 7 bytes: a byte of their top bits (bit i: byte i's), then their low 7 bits.
inline std::size_t Pack7(const uint8_t* in, std::size_t length, uint8_t* out)
{
    std::size_t at = 0;
    for (std::size_t group = 0; group < length; group += 7)
    {
        const std::size_t count = length - group < 7 ? length - group : 7;
        uint8_t high = 0;
        for (std::size_t index = 0; index < count; ++index)
            if (in[group + index] & 0x80) high |= static_cast<uint8_t>(1u << index);
        out[at++] = high;
        for (std::size_t index = 0; index < count; ++index) out[at++] = in[group + index] & 0x7F;
    }
    return at;
}

// Returns the bytes unpacked (at most `capacity`).
inline std::size_t Unpack7(const uint8_t* in, std::size_t length, uint8_t* out, std::size_t capacity)
{
    std::size_t at = 0;
    for (std::size_t group = 0; group < length; group += 8)
    {
        const uint8_t high = in[group];
        for (std::size_t index = 1; index < 8 && group + index < length && at < capacity; ++index)
            out[at++] = static_cast<uint8_t>((in[group + index] & 0x7F) | ((high >> (index - 1)) & 1u) << 7);
    }
    return at;
}

// A HID answer, report 0xCB's 63 bytes: C5 id seq_lo seq_hi flags length payload…
inline void HidAnswer(uint8_t id, uint16_t seq, uint8_t flags, const uint8_t* payload, std::size_t length, uint8_t out[kReport])
{
    std::memset(out, 0, kReport);
    if (length > kHidPayload) length = kHidPayload;
    out[0] = kAnswerCommand | 0x80;
    out[1] = id & 0x7F;
    out[2] = seq & 0x7F;
    out[3] = (seq >> 7) & 0x7F;
    out[4] = flags & 0x7F;
    out[5] = static_cast<uint8_t>(length);
    if (length != 0) std::memcpy(out + 6, payload, length);
}

// A SysEx answer, whole: F0 24 45 id seq_lo seq_hi flags <payload, packed> F7. Returns its length.
inline std::size_t SysexAnswer(uint8_t id, uint16_t seq, uint8_t flags, const uint8_t* payload, std::size_t length, uint8_t* out)
{
    if (length > kSysexPayload) length = kSysexPayload;
    std::size_t at = 0;
    out[at++] = 0xF0;
    out[at++] = 0x24;
    out[at++] = kAnswerCommand;
    out[at++] = id & 0x7F;
    out[at++] = seq & 0x7F;
    out[at++] = (seq >> 7) & 0x7F;
    out[at++] = flags & 0x7F;
    at += Pack7(payload, length, out + at);
    out[at++] = 0xF7;
    return at;
}

inline uint32_t ReadU32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8 | static_cast<uint32_t>(bytes[2]) << 16 |
           static_cast<uint32_t>(bytes[3]) << 24;
}
inline void WriteU32(uint8_t* bytes, uint32_t value)
{
    for (int index = 0; index < 4; ++index) bytes[index] = static_cast<uint8_t>(value >> (8 * index));
}

// A path on the card from a request: "/sdcard" + "/" + path. Names between '/', none empty, ".", ".." or
// with a control character or '\\'; no leading '/'; at most kMaxPath bytes. The empty path is the root when
// `rootAllowed`. False for anything else.
inline bool CardPath(const char* root, const uint8_t* path, std::size_t length, bool rootAllowed, char* out, std::size_t capacity)
{
    if (length > kMaxPath) return false;
    if (length == 0 && !rootAllowed) return false;
    const std::size_t rootLength = std::strlen(root);
    if (rootLength + 1 + length + 1 > capacity) return false;
    std::size_t start = 0;
    for (std::size_t at = 0; at <= length; ++at)
    {
        if (at < length && path[at] != '/')
        {
            if (path[at] < 0x20 || path[at] == 0x7F || path[at] == '\\') return false;
            continue;
        }
        const std::size_t name = at - start;
        if (length != 0)
        {
            if (name == 0) return false;  // a leading, trailing or doubled '/'
            if (name == 1 && path[start] == '.') return false;
            if (name == 2 && path[start] == '.' && path[start + 1] == '.') return false;
        }
        start = at + 1;
    }
    std::memcpy(out, root, rootLength);
    std::size_t at = rootLength;
    if (length != 0)
    {
        out[at++] = '/';
        std::memcpy(out + at, path, length);
        at += length;
    }
    out[at] = 0;
    return true;
}

// The run-length code of SCREEN: [count 1..255, pixel u16 little-endian] pairs.
inline std::size_t RleSize(const uint16_t* pixels, std::size_t count)
{
    std::size_t size = 0;
    for (std::size_t at = 0; at < count;)
    {
        std::size_t run = 1;
        while (at + run < count && run < 255 && pixels[at + run] == pixels[at]) ++run;
        size += 3;
        at += run;
    }
    return size;
}
template <class Put>
bool RleEncode(const uint16_t* pixels, std::size_t count, Put&& put)
{
    for (std::size_t at = 0; at < count;)
    {
        std::size_t run = 1;
        while (at + run < count && run < 255 && pixels[at + run] == pixels[at]) ++run;
        const uint8_t pair[3] = {static_cast<uint8_t>(run), static_cast<uint8_t>(pixels[at]), static_cast<uint8_t>(pixels[at] >> 8)};
        if (!put(pair, sizeof(pair))) return false;
        at += run;
    }
    return true;
}

// An application image's length from its headers (esp_image_format.h, read through `read`): the 24-byte
// header, each segment's 8-byte header and data, the checksum byte that pads it to 16 bytes, and the SHA-256
// after it when the header says one is appended. 0 when there is no image there (wrong magic, a segment out
// of `limit`). `read(offset, out, bytes)` reads the partition.
template <class Read>
uint32_t AppImageLength(Read&& read, uint32_t limit)
{
    uint8_t header[24];
    if (limit < sizeof(header) || !read(0, header, sizeof(header))) return 0;
    if (header[0] != 0xE9) return 0;
    const unsigned segments = header[1];
    if (segments == 0 || segments > 16) return 0;
    uint32_t at = sizeof(header);
    for (unsigned segment = 0; segment < segments; ++segment)
    {
        uint8_t segmentHeader[8];
        if (at + sizeof(segmentHeader) > limit || !read(at, segmentHeader, sizeof(segmentHeader))) return 0;
        const uint32_t length = ReadU32(segmentHeader + 4);
        at += sizeof(segmentHeader);
        if (length > limit - at) return 0;
        at += length;
    }
    at = (at | 15u) + 1;  // the checksum byte, padded to 16
    if (header[23] == 1) at += 32;
    return at <= limit ? at : 0;
}

// One NVS page from its first 64 bytes: in use (active or full) and how many of its 126 entries are written.
struct NvsPage {
    bool used = false;
    uint16_t written = 0;
};
constexpr uint16_t kNvsEntriesPerPage = 126;
inline NvsPage ReadNvsPage(const uint8_t* header)
{
    NvsPage page;
    const uint32_t state = ReadU32(header);
    page.used = state == 0xFFFFFFFE || state == 0xFFFFFFFC;  // active, full
    if (!page.used) return page;
    for (uint16_t entry = 0; entry < kNvsEntriesPerPage; ++entry)
    {
        const uint8_t bits = (header[32 + entry / 4] >> (2 * (entry % 4))) & 3u;
        if (bits == 2) ++page.written;  // 3 empty, 2 written, 0 erased
    }
    return page;
}

// JSON into `sink(const char*, size_t)` (false: stop), commas and nesting kept by the writer.
template <class Sink>
class Json {
  public:
    explicit Json(Sink& sink) : sink_(sink) {}

    Json& Open(char bracket) { Comma(); Raw(&bracket, 1); Push(); return *this; }
    Json& Close(char bracket) { if (depth_ > 0) --depth_; Raw(&bracket, 1); first_[depth_] = false; return *this; }
    Json& Object() { return Open('{'); }
    Json& Array() { return Open('['); }
    Json& End(char bracket = '}') { return Close(bracket); }
    Json& Key(const char* key)
    {
        Comma();
        String(key);
        Raw(":", 1);
        keyed_ = true;
        return *this;
    }
    Json& Str(const char* text, std::size_t length = static_cast<std::size_t>(-1))
    {
        Comma();
        String(text, length);
        return *this;
    }
    Json& Int(long long value)
    {
        Comma();
        char text[24];
        const int length = std::snprintf(text, sizeof(text), "%lld", value);
        Raw(text, static_cast<std::size_t>(length));
        return *this;
    }
    Json& Uint(unsigned long long value)
    {
        Comma();
        char text[24];
        const int length = std::snprintf(text, sizeof(text), "%llu", value);
        Raw(text, static_cast<std::size_t>(length));
        return *this;
    }
    Json& Fixed(double value, int decimals)
    {
        Comma();
        char text[32];
        const int length = std::snprintf(text, sizeof(text), "%.*f", decimals, value);
        Raw(text, static_cast<std::size_t>(length));
        return *this;
    }
    Json& Bool(bool value) { Comma(); Raw(value ? "true" : "false", value ? 4 : 5); return *this; }
    Json& Null() { Comma(); Raw("null", 4); return *this; }
    bool Ok() const { return ok_; }

  private:
    void Push()
    {
        if (depth_ + 1 < sizeof(first_)) ++depth_;
        first_[depth_] = true;
    }
    void Comma()
    {
        if (keyed_)
        {
            keyed_ = false;
            return;
        }
        if (!first_[depth_]) Raw(",", 1);
        first_[depth_] = false;
    }
    void Raw(const char* text, std::size_t length)
    {
        if (ok_ && length != 0) ok_ = sink_(text, length);
    }
    void String(const char* text, std::size_t length = static_cast<std::size_t>(-1))
    {
        if (text == nullptr) text = "";
        if (length == static_cast<std::size_t>(-1)) length = std::strlen(text);
        Raw("\"", 1);
        std::size_t start = 0;
        for (std::size_t at = 0; at < length; ++at)
        {
            const unsigned char c = static_cast<unsigned char>(text[at]);
            if (c >= 0x20 && c != '"' && c != '\\') continue;
            Raw(text + start, at - start);
            char escape[8];
            const int escaped = c == '"' ? std::snprintf(escape, sizeof(escape), "\\\"")
                                : c == '\\' ? std::snprintf(escape, sizeof(escape), "\\\\")
                                : c == '\n' ? std::snprintf(escape, sizeof(escape), "\\n")
                                            : std::snprintf(escape, sizeof(escape), "\\u%04x", c);
            Raw(escape, static_cast<std::size_t>(escaped));
            start = at + 1;
        }
        Raw(text + start, length - start);
        Raw("\"", 1);
    }

    Sink& sink_;
    bool ok_ = true;
    bool keyed_ = false;
    std::size_t depth_ = 0;
    bool first_[12] = {true};
};

}  // namespace CardenzaHAL::USB::Dev
