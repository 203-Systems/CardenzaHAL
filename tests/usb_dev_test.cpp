// usb/dev_logic.h (the development channel's framing, packing, run-length code, JSON, card paths, image
// length and NVS pages) and keyboard/internal/inject.h's key numbers (docs/usb-dev.md).
#include "keyboard/internal/inject.h"
#include "usb/dev_logic.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace CardenzaHAL::USB::Dev;

namespace {

void PackTests()
{
    // Round trips of every length up to a request's and an answer's, with every top bit pattern.
    for (std::size_t length = 0; length <= 60; ++length)
    {
        std::vector<uint8_t> in(length);
        for (std::size_t index = 0; index < length; ++index) in[index] = static_cast<uint8_t>(index * 37 + 0x80 * (index % 3 == 0));
        std::vector<uint8_t> packed(PackedSize(length) + 1, 0xEE);
        const std::size_t size = Pack7(in.data(), length, packed.data());
        assert(size == PackedSize(length));
        for (std::size_t index = 0; index < size; ++index) assert(packed[index] < 0x80);
        std::vector<uint8_t> out(length + 8);
        assert(Unpack7(packed.data(), size, out.data(), out.size()) == length);
        assert(std::memcmp(in.data(), out.data(), length) == 0);
    }
    // The layout: a byte of top bits first, bit i for byte i.
    const uint8_t in[8] = {0x81, 0x02, 0xFF, 0x00, 0x00, 0x00, 0x80, 0x7F};
    uint8_t packed[10];
    assert(Pack7(in, 8, packed) == 10);
    const uint8_t expected[10] = {0x45, 0x01, 0x02, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F};
    assert(std::memcmp(packed, expected, sizeof(expected)) == 0);
    // The request limit: 60 packed bytes after F0 23 45 id op give 52.
    assert(PackedSize(kMaxArgs) <= 60 && PackedSize(kMaxArgs + 1) > 60);
    uint8_t capped[4];
    assert(Unpack7(packed, 10, capped, sizeof(capped)) == 4);
}

void FrameTests()
{
    const uint8_t payload[3] = {'a', 0x80, 'c'};
    uint8_t report[kReport];
    HidAnswer(5, 0x3FFF, kFlagLast, payload, 3, report);
    const uint8_t head[9] = {0xC5, 5, 0x7F, 0x7F, kFlagLast, 3, 'a', 0x80, 'c'};
    assert(std::memcmp(report, head, sizeof(head)) == 0);
    for (std::size_t index = sizeof(head); index < kReport; ++index) assert(report[index] == 0);
    // A full payload fills the report exactly.
    std::vector<uint8_t> full(kHidPayload, 0xAB);
    HidAnswer(1, 130, 0, full.data(), full.size() + 10, report);
    assert(report[2] == (130 & 0x7F) && report[3] == 1 && report[5] == kHidPayload && report[kReport - 1] == 0xAB);

    uint8_t message[kSysexMessage + 4];
    const std::size_t size = SysexAnswer(9, 200, kFlagLast | kFlagError, payload, 3, message);
    assert(size == 7 + PackedSize(3) + 1);
    const uint8_t sysex[] = {0xF0, 0x24, 0x45, 9, 200 & 0x7F, 1, 3, 0x02, 'a', 0x00, 'c', 0xF7};
    assert(size == sizeof(sysex) && std::memcmp(message, sysex, size) == 0);
    std::vector<uint8_t> most(kSysexPayload, 0xFF);
    assert(SysexAnswer(1, 0, 0, most.data(), most.size(), message) == kSysexMessage);
    assert(kSysexMessage <= kReport);  // what the HAL's SysEx answers were already
    for (std::size_t index = 1; index + 1 < kSysexMessage; ++index) assert(message[index] < 0x80);
}

void PathTests()
{
    char out[300];
    const auto path = [&](const char* text, bool root) {
        return CardPath("/sdcard", reinterpret_cast<const uint8_t*>(text), std::strlen(text), root, out, sizeof(out));
    };
    assert(path("CardenzaSynth/Logs/log.txt", false) && std::string(out) == "/sdcard/CardenzaSynth/Logs/log.txt");
    assert(path("", true) && std::string(out) == "/sdcard");
    assert(!path("", false));
    assert(path("a b/c.d..e", false));
    for (const char* bad : {"/abs", "a//b", "a/", "..", "a/../b", "./a", "a/.", "a\\b", "a\nb"}) assert(!path(bad, true));
    const std::string longest(kMaxPath, 'x');
    assert(path(longest.c_str(), false));
    assert(!path((longest + "x").c_str(), false));
    char small[12];
    assert(!CardPath("/sdcard", reinterpret_cast<const uint8_t*>("abcd"), 4, false, small, sizeof(small)));
}

void RleTests()
{
    std::vector<uint16_t> pixels(600, 0x1234);
    pixels[0] = 0xF800;
    for (std::size_t index = 300; index < 310; ++index) pixels[index] = static_cast<uint16_t>(index);
    std::vector<uint8_t> code;
    assert(RleEncode(pixels.data(), pixels.size(), [&](const uint8_t* bytes, std::size_t length) {
        code.insert(code.end(), bytes, bytes + length);
        return true;
    }));
    assert(code.size() == RleSize(pixels.data(), pixels.size()));
    std::vector<uint16_t> back;
    for (std::size_t at = 0; at + 3 <= code.size(); at += 3)
    {
        assert(code[at] >= 1);
        back.insert(back.end(), code[at], static_cast<uint16_t>(code[at + 1] | code[at + 2] << 8));
    }
    assert(back == pixels);
    // Runs stop at 255.
    const std::vector<uint16_t> flat(1000, 7);
    assert(RleSize(flat.data(), flat.size()) == 3 * 4);
    // A writer that stops stops the encoder.
    int calls = 0;
    assert(!RleEncode(pixels.data(), pixels.size(), [&](const uint8_t*, std::size_t) { return ++calls < 2; }));
    assert(calls == 2);
}

void ImageTests()
{
    // Header (24), two segments (8 + 100, 8 + 13), checksum padded to 16, then the SHA-256.
    std::vector<uint8_t> image(512, 0);
    image[0] = 0xE9;
    image[1] = 2;
    image[23] = 1;
    WriteU32(&image[24 + 4], 100);
    WriteU32(&image[24 + 8 + 100 + 4], 13);
    const auto read = [&](uint32_t offset, void* out, std::size_t length) {
        if (offset + length > image.size()) return false;
        std::memcpy(out, image.data() + offset, length);
        return true;
    };
    const uint32_t end = 24 + 8 + 100 + 8 + 13;  // 153
    assert(AppImageLength(read, 512) == ((end | 15u) + 1) + 32);  // 160 + 32
    image[23] = 0;
    assert(AppImageLength(read, 512) == 160);
    assert(AppImageLength(read, 159) == 0);  // past the partition
    image[0] = 0xFF;
    assert(AppImageLength(read, 512) == 0);  // erased
    image[0] = 0xE9;
    WriteU32(&image[24 + 4], 0xFFFFFF00);
    assert(AppImageLength(read, 512) == 0);  // a segment out of bounds
    image[1] = 0;
    assert(AppImageLength(read, 512) == 0);
}

void NvsTests()
{
    uint8_t header[64];
    std::memset(header, 0xFF, sizeof(header));
    assert(!ReadNvsPage(header).used);  // empty page
    WriteU32(header, 0xFFFFFFFE);       // active
    assert(ReadNvsPage(header).used && ReadNvsPage(header).written == 0);
    header[32] = 0xFF & ~0x03 & ~0x0C;  // entries 0 and 1: 00 erased
    header[32] = static_cast<uint8_t>((header[32] & ~0x03) | 0x02);  // entry 0 written
    header[33] = static_cast<uint8_t>((0xFF & ~0xC0) | 0x80);       // entry 7 written
    header[63] = static_cast<uint8_t>((0xFF & ~0x30) | 0x20);       // entry 126 would be: not counted
    header[63] = static_cast<uint8_t>((header[63] & ~0x0C) | 0x08); // entry 125 written
    assert(ReadNvsPage(header).written == 3);
    WriteU32(header, 0xFFFFFFFC);  // full
    assert(ReadNvsPage(header).used && ReadNvsPage(header).written == 3);
    WriteU32(header, 0xFFFFFFF8);  // being freed
    assert(!ReadNvsPage(header).used);
}

void JsonTests()
{
    std::string text;
    auto sink = [&](const char* data, std::size_t length) {
        text.append(data, length);
        return true;
    };
    Json<decltype(sink)> json(sink);
    json.Object().Key("a").Int(-3).Key("b").Str("q\"\\\n\x01é").Key("c").Array().Uint(1).Bool(false).Null().Object().End().End(']');
    json.Key("d").Fixed(12.345, 1).Key("e").Object().Key("f").Array().End(']').End().End();
    assert(json.Ok());
    assert(text == "{\"a\":-3,\"b\":\"q\\\"\\\\\\n\\u0001é\",\"c\":[1,false,null,{}],\"d\":12.3,\"e\":{\"f\":[]}}");
    // A sink that refuses stops the writer.
    int calls = 0;
    auto refusing = [&](const char*, std::size_t) { return ++calls < 3; };
    Json<decltype(refusing)> stopped(refusing);
    stopped.Object().Key("a").Int(1).Key("b").Int(2).End();
    assert(!stopped.Ok() && calls == 3);
    // Lengths given.
    text.clear();
    Json<decltype(sink)> sized(sink);
    sized.Array().Str("abcdef", 3).End(']');
    assert(text == "[\"abc\"]");
}

void KeyTests()
{
    using CardenzaHAL::Keyboard::detail::KeyScanSlot;
    int row = 0, column = 0;
    assert(!KeyScanSlot(0, row, column) && !KeyScanSlot(57, row, column));
    // Every key number has its own slot, and the scan's position for it gives the number back
    // (KeyPosition in keyboard_key_scan.cpp).
    bool used[8][7] = {};
    for (int number = 1; number <= 56; ++number)
    {
        assert(KeyScanSlot(number, row, column));
        assert(row >= 0 && row < 8 && column >= 0 && column < 7);
        assert(!used[row][column]);
        used[row][column] = true;
        const auto& chart = CardenzaHAL::Keyboard::X_map_chart[column];
        const int x = row > 3 ? chart.x_1 : chart.x_2;
        const int y = 3 - (row > 3 ? row - 4 : row);
        assert(y * 14 + x + 1 == number);
    }
    // ESC (1) is top left, the last key (56) bottom right.
    assert(KeyScanSlot(1, row, column) && row == 7 && column == 0);
    assert(KeyScanSlot(56, row, column) && row == 0 && column == 6);
}

}  // namespace

// The board's settings on the channel (docs/usb-dev.md): their ops after the files', protocol 3, SET's value.
void SettingsOpTests()
{
    using namespace CardenzaHAL::USB::Dev;
    static_assert(kSettings == 0x40 && kSettingGet == 0x41 && kSettingSet == 0x42 && kSettingsReset == 0x43);
    static_assert(kSettings > kMv && kSettingsReset < kCancel);
    static_assert(kProtocol == 3);
    const uint8_t value[] = {0x50, 0x00, 0x00, 0x00, 0xF4, 0xFF, 0xFF, 0xFF};
    assert(ReadI32(value) == 80);
    assert(ReadI32(value + 4) == -12);
}

int main()
{
    PackTests();
    SettingsOpTests();
    FrameTests();
    PathTests();
    RleTests();
    ImageTests();
    NvsTests();
    JsonTests();
    KeyTests();
    std::cout << "usb dev tests passed\n";
    return 0;
}
