// net/: the Launcher's storage of saved networks (CRC-32 keys, base64, AES-128-CBC), the request
// checks, the order networks are tried in and the NVS page check. Pure code, no ESP-IDF.
#include "net/connect_order.h"
#include "net/http_request.h"
#include "net/internal/nvs_page.h"
#include "net/launcher_settings.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
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

namespace Net = CardenzaHAL::Net;
namespace Wifi = CardenzaHAL::Net::LauncherWifi;

void Crc32Tests()
{
    // The CRC-32 check value, and values from Python's binascii.crc32 (zlib).
    CHECK(Wifi::Crc32("123456789", 9) == 0xCBF43926u);
    CHECK(Wifi::Crc32("", 0) == 0);
    CHECK(Wifi::Crc32("a", 1) == 0xE8B7BE43u);
    CHECK(Wifi::Crc32("HomeNet", 7) == 0x4B96672Fu);
    const char utf8[] = "Caf\xC3\xA9 \xE5\x92\x96\xE5\x95\xA1";
    CHECK(Wifi::Crc32(utf8, std::strlen(utf8)) == 0xFDD4DB45u);

    // The Launcher's makeWifiKey: "%c_%08X".
    char key[11];
    Wifi::EntryKey('s', "123456789", key);
    CHECK(std::strcmp(key, "s_CBF43926") == 0);
    Wifi::EntryKey('p', "HomeNet", key);
    CHECK(std::strcmp(key, "p_4B96672F") == 0);
    Wifi::EntryKey('b', "a", key);
    CHECK(std::strcmp(key, "b_E8B7BE43") == 0);
    Wifi::EntryKey('s', "", key);
    CHECK(std::strcmp(key, "s_00000000") == 0);
}

bool Decodes(const char* text, const char* expected)
{
    uint8_t out[64];
    std::size_t written = 99;
    if (!Wifi::Base64Decode(text, std::strlen(text), out, sizeof(out), written)) return false;
    return written == std::strlen(expected) && std::memcmp(out, expected, written) == 0;
}

bool Refuses(const char* text, std::size_t capacity = 64)
{
    uint8_t out[64];
    std::size_t written = 99;
    const bool ok = Wifi::Base64Decode(text, std::strlen(text), out, capacity, written);
    return !ok && written == 0;
}

void Base64Tests()
{
    // RFC 4648 section 10.
    CHECK(Decodes("", ""));
    CHECK(Decodes("Zg==", "f"));
    CHECK(Decodes("Zm8=", "fo"));
    CHECK(Decodes("Zm9v", "foo"));
    CHECK(Decodes("Zm9vYg==", "foob"));
    CHECK(Decodes("Zm9vYmE=", "fooba"));
    CHECK(Decodes("Zm9vYmFy", "foobar"));
    CHECK(Decodes("+/+/", "\xFB\xFF\xBF"));
    // Not standard base64 with padding, or not fitting.
    CHECK(Refuses("Zg"));         // no padding
    CHECK(Refuses("Zg="));        // length not a multiple of 4
    CHECK(Refuses("Zg=x"));       // '=' in the middle
    CHECK(Refuses("Z==="));       // three padding characters
    CHECK(Refuses("===="));
    CHECK(Refuses("Zm9v\nYmFy")); // line break
    CHECK(Refuses("Zm9v YmFy"));
    CHECK(Refuses("Zm9-"));       // the URL alphabet
    CHECK(Refuses("Zm9_"));
    CHECK(Refuses("Zh=="));       // bits after the last byte that are not zero
    CHECK(Refuses("Zm9="));
    CHECK(Refuses("Zg==Zg=="));   // padding before the end
    CHECK(Refuses("Zm9vYmFy", 5)); // 6 bytes into 5
    uint8_t out[8];
    std::size_t written = 0;
    CHECK(Wifi::Base64Decode("Zm9vYmFy", 8, out, 6, written) && written == 6);
}

struct Vector {
    const char* plain;
    std::size_t length;
    const char* cipher;
};

// Made by the 203 Launcher's own code: 203-launcher src/wifi_crypto.cpp (wifiPwdEncrypt, with
// WIFI_ENC_KEY "LauncherWifi2025" as main/CMakeLists.txt sets it) compiled on a computer with its
// platform/text.h String, against the mbedTLS of ESP-IDF 5.3.5 (aes.c, base64.c), each checked to come
// back through the Launcher's wifiPwdDecrypt. Lengths are bytes.
const Vector kLauncherVectors[] = {
    {"", 0, ""},
    {"a", 1, "Njj3M7N+UXJ9M9orRz+EYw=="},
    {"password", 8, "yE1BBkbMKl1PmONXwFUmlw=="},
    {"my home wifi 15", 15, "tzPSiD/5PCgKUQeVdklifQ=="},
    {"!#$%&'()*+,-./:", 15, "8F0mbmLxaPaREeMDqpWG8A=="},
    {"16 chars exactly", 16, "2vkpnCMb10mY9RtDa5EhYL0Ecfm8M/ThBL+w+z6Ge2c="},
    {"seventeen chars!!", 17, "2EkN595KWSoF/M1qSxs7lyKg8YICa2aGpwnDsG9qR4U="},
    {"{\"quoted\"}\\<tab>\t|~`^@[]_=?;", 28, "xTycvGXfW4I/pQfi96nWyRZbT4V80hBARtn45JE/O0k="},
    {"0123456789abcdefghijklmnopqrstuv", 32, "Xc1fSsGcW8T27vNKgA6w+v7UW0tz6rPe2x0hAufSUiveGt7wZac4/oI+WOaX9IMN"},
    {"Caf\xC3\xA9 \xE5\x92\x96\xE5\x95\xA1 \xE2\x98\x95 n\xC3\xBCrnberg", 26, "cLAkZiju47WgpQ/n9QgfBsUfwdchBgmrkn9+f8m75jY="},
    {"The quick brown fox jumps over the lazy dog, then naps. 1234567", 63,
     "NkyDelO5TwF08pT+cTImDsz5JuT4kHv7uhSCklXe604hyxJoJ2p6OJ7IiGAh0dyL2Q3fhkKVjNzw/2i2VWKxGQ=="},
};

// Cipher texts the Launcher's encrypt never writes: AES-128-CBC with its key and IV over hand-made
// blocks (same generator, mbedTLS), then base64. Its wifiPwdDecrypt refuses all but the noted one.
const char* const kRefused[] = {
    // 64 characters: 80 bytes, 108 characters of base64 (over the 88 the Launcher accepts).
    "72bOwaprwGyNQVgSTVyovJUayN3KsxuorVbOJvOjX6eM6xy1tQJsj5vJuecmkLBGO3TazLRk38Macq5MsVIt96HwdteIqN6/ViLPlBaEC1E=",
    "ddICjjRofoR7xHK3xKzm3Q==",                     // padding byte 0
    "3QzOqNIZN5bys6h0YbjtiZj+Cy2xR+VJTH6i3GYec4U=", // padding byte 17
    "29ASHBy+glEA+GfalfIpDw==",                     // padding bytes 03 02 03
    "/4PiMLyBM7QwbG9vTb/VvA==",                     // a whole block of padding (16 x 0x10, nothing before)
    "AQAAAAAAAAAAAAAAAAAA",                         // 15 bytes: not whole blocks
    // A zero byte inside ("cc\0cc" + padding): the Launcher's decrypt accepts it (its String keeps the
    // zero), a C string cannot: refused here.
    "IQwrtzx3ohJmplbspSzQEQ==",
};

void DecryptTests()
{
    for (const Vector& vector : kLauncherVectors)
    {
        char plain[64];
        std::memset(plain, 'x', sizeof(plain));
        CHECK(Wifi::DecryptPassword(vector.cipher, plain, sizeof(plain)));
        CHECK(std::strlen(plain) == vector.length);
        CHECK(std::memcmp(plain, vector.plain, vector.length) == 0);
    }
    // 16 characters and a whole block of padding after them: accepted (the same generator).
    char plain[64];
    CHECK(Wifi::DecryptPassword("Fb8QxfM7gg3zKsOrdaNnBqd7ND4zHu4D7DMXOxx2wq0=", plain, sizeof(plain)));
    CHECK(std::strcmp(plain, "bbbbbbbbbbbbbbbb") == 0);

    for (const char* cipher : kRefused)
    {
        std::memset(plain, 'x', sizeof(plain));
        CHECK(!Wifi::DecryptPassword(cipher, plain, sizeof(plain)));
        CHECK(plain[0] == 0);
    }
    // Not base64 at all, a cut-off text, one character too many, over 88 characters.
    for (const char* cipher : {"not base64!", "yE1BBkbMKl1PmONXwFUml", "yE1BBkbMKl1PmONXwFUmlw==A",
                               "yE1BBkbMKl1PmONXwFUmlw==\n"})
    {
        std::memset(plain, 'x', sizeof(plain));
        CHECK(!Wifi::DecryptPassword(cipher, plain, sizeof(plain)));
        CHECK(plain[0] == 0);
    }
    const std::string long89(89, 'A');
    CHECK(!Wifi::DecryptPassword(long89.c_str(), plain, sizeof(plain)) && plain[0] == 0);
    // A cipher that is fine but changed in one character decrypts to something else; its padding is
    // then almost surely wrong (this one: checked to be refused).
    CHECK(!Wifi::DecryptPassword("yE1BBkbMKl1PmONXwFUmlx==", plain, sizeof(plain)));
    // The password does not fit the caller's buffer: refused whole, never cut.
    CHECK(!Wifi::DecryptPassword("yE1BBkbMKl1PmONXwFUmlw==", plain, 8) && plain[0] == 0);
    CHECK(Wifi::DecryptPassword("yE1BBkbMKl1PmONXwFUmlw==", plain, 9) && std::strcmp(plain, "password") == 0);
    CHECK(!Wifi::DecryptPassword(nullptr, plain, sizeof(plain)) && plain[0] == 0);
    CHECK(!Wifi::DecryptPassword("", nullptr, 0));

    // Forget clears every password, nothing else.
    Net::LauncherSettings settings;
    std::strcpy(settings.networks[0].ssid, "HomeNet");
    std::strcpy(settings.networks[0].password, "password");
    std::strcpy(settings.networks[15].password, "another one");
    std::strcpy(settings.account.token, "ccd_x");
    settings.Forget();
    for (const auto& network : settings.networks)
        for (char c : network.password) CHECK(c == 0);
    CHECK(std::strcmp(settings.networks[0].ssid, "HomeNet") == 0);
    CHECK(std::strcmp(settings.account.token, "ccd_x") == 0);
}

void AccountTests()
{
    // As the Launcher's loadAccount (203-launcher src/community.cpp, communityValidToken): "ccd_" and
    // exactly 64 lower-case hex digits, and an API URL.
    Net::LauncherAccount account;
    const std::string token = "ccd_" + std::string(32, 'a') + std::string(32, '0');
    std::strcpy(account.token, token.c_str());
    std::strcpy(account.api, "https://community.203.io/api");
    CHECK(Net::ValidBinding(account) && account.Bound());
    account.api[0] = 0;
    CHECK(!Net::ValidBinding(account));
    std::strcpy(account.api, "https://community.203.io/api");
    for (const std::string& bad : {std::string("cct_") + std::string(64, 'a'), "ccd_" + std::string(63, 'a'),
                                   "ccd_" + std::string(65, 'a'), "ccd_" + std::string(63, 'a') + "A",
                                   "ccd_" + std::string(63, 'a') + "g", std::string("ccd_"), std::string()})
    {
        std::strcpy(account.token, bad.c_str());
        CHECK(!Net::ValidBinding(account));
    }

    // Names are cut before a character that does not fit whole.
    char out[8];
    Net::CopyUtf8("abc", out, sizeof(out));
    CHECK(std::strcmp(out, "abc") == 0);
    Net::CopyUtf8("abcdefgh", out, sizeof(out));
    CHECK(std::strcmp(out, "abcdefg") == 0);
    Net::CopyUtf8("ab\xE5\x92\x96\xE5\x95\xA1", out, sizeof(out));  // 2 + 3 + 3 bytes: the second character does not fit
    CHECK(std::strcmp(out, "ab\xE5\x92\x96") == 0);
    Net::CopyUtf8("abcdef\xC3\xA9", out, sizeof(out));  // a 2-byte character from byte 6 of 7
    CHECK(std::strcmp(out, "abcdef") == 0);
    Net::CopyUtf8("\xF0\x9F\x8E\xB9\xF0\x9F\x8E\xB9", out, sizeof(out));  // two 4-byte characters
    CHECK(std::strcmp(out, "\xF0\x9F\x8E\xB9") == 0);
    Net::CopyUtf8(nullptr, out, sizeof(out));
    CHECK(out[0] == 0);
    char one[1];
    Net::CopyUtf8("abc", one, 1);
    CHECK(one[0] == 0);
    // The Launcher keeps 64 bytes (communityLine); the field keeps 63.
    const std::string name64 = std::string(61, 'n') + "\xE5\x92\x96";
    Net::CopyUtf8(name64.c_str(), account.userName, sizeof(account.userName));
    CHECK(std::strlen(account.userName) == 61);
}

void UrlTests()
{
    Net::UrlParts parts;
    CHECK(Net::ParseUrl("https://community.203.io/api/", parts));
    CHECK(parts.https && parts.port == 443);
    CHECK(std::string(parts.host, parts.hostLength) == "community.203.io");
    CHECK(std::string(parts.path, parts.pathLength) == "/api/");
    CHECK(Net::ParseUrl("http://192.168.1.20:8080/upload?name=a%20b&x=1", parts));
    CHECK(!parts.https && parts.port == 8080);
    CHECK(std::string(parts.host, parts.hostLength) == "192.168.1.20");
    CHECK(std::string(parts.path, parts.pathLength) == "/upload?name=a%20b&x=1");
    CHECK(Net::ParseUrl("HTTPS://Example.COM", parts));
    CHECK(parts.https && parts.port == 443 && std::string(parts.path, parts.pathLength) == "/");
    CHECK(Net::ParseUrl("http://a", parts) && parts.port == 80);
    CHECK(Net::ParseUrl("https://x.supabase.co:65535/storage/v1/object/upload/sign/a/b.wav?token=eyJ.a-b_c", parts));
    CHECK(parts.port == 65535);

    for (const char* bad : {
             "", "community.203.io/api", "ftp://example.com/", "https:/example.com", "https://",
             "https:///path", "https://user:pass@example.com/", "https://user@example.com/",
             "https://example.com:/", "https://example.com:0/", "https://example.com:65536/",
             "https://example.com:123456/", "https://example.com:8a/", "https://[::1]/", "https://exa mple.com/",
             "https://example.com/a b", "https://example.com/a#frag", "https://example.com#frag",
             "https://example.com?q=1", "https://example.com/\x01", "https://example.com/\x7F",
             "https://example.com/caf\xC3\xA9", "https://.example.com/", "https://example..com/",
             "https://example.com./", "https://example_com/", "https://example.com/path\r\nX: y",
         })
        CHECK(!Net::ParseUrl(bad, parts));
    CHECK(!Net::ParseUrl(nullptr, parts));
    const std::string longHost = "https://" + std::string(254, 'a') + "/";
    CHECK(!Net::ParseUrl(longHost.c_str(), parts));
}

void HeaderTests()
{
    for (const char* name : {"Authorization", "Content-Type", "X-Firmware-Unlock", "x_y", "A!#$%&'*+-.^_`|~9"})
        CHECK(Net::ValidHeaderName(name));
    for (const char* name : {"", "Bad Name", "Bad:Name", "Bad\r\nName", "(x)", "a/b", "caf\xC3\xA9"})
        CHECK(!Net::ValidHeaderName(name));
    CHECK(!Net::ValidHeaderName(nullptr));

    for (const char* value : {"", "Bearer ccd_0123", "application/json; charset=utf-8", "a\tb", "caf\xC3\xA9", "~!@#"})
        CHECK(Net::ValidHeaderValue(value));
    for (const char* value : {"a\r\nX-Injected: 1", "a\rb", "a\nb", "a\x01", "a\x7F"}) CHECK(!Net::ValidHeaderValue(value));
    CHECK(!Net::ValidHeaderValue(nullptr));

    for (const char* method : {"GET", "POST", "PUT", "DELETE", "HEAD", "PATCH"}) CHECK(Net::ValidMethod(method));
    for (const char* method : {"get", "OPTIONS", "CONNECT", "TRACE", "", "GET ", "POSTX"}) CHECK(!Net::ValidMethod(method));
    CHECK(!Net::ValidMethod(nullptr));
}

void RequestTests()
{
    Net::Request request;
    request.url = "https://community.203.io/api/me";
    CHECK(Net::ValidRequest(request));
    request.method = "BREW";
    CHECK(!Net::ValidRequest(request));
    request.method = "POST";
    request.url = "https://community.203.io/api/me#x";
    CHECK(!Net::ValidRequest(request));
    request.url = "https://community.203.io/api/device/works";

    const Net::Header good[] = {{"Authorization", "Bearer ccd_0123"}, {"Content-Type", "application/json"}};
    request.headers = good;
    request.headerCount = 2;
    CHECK(Net::ValidRequest(request));
    request.headers = nullptr;
    CHECK(!Net::ValidRequest(request));
    const Net::Header injected[] = {{"X-Name", "a\r\nHost: evil"}};
    request.headers = injected;
    request.headerCount = 1;
    CHECK(!Net::ValidRequest(request));
    // Headers the HAL sets itself, in any case.
    for (const char* own : {"Host", "content-length", "TRANSFER-ENCODING", "Connection"})
    {
        const Net::Header header[] = {{own, "x"}};
        request.headers = header;
        CHECK(!Net::ValidRequest(request));
    }
    request.headers = nullptr;
    request.headerCount = 0;

    // A body from memory.
    const char json[] = "{\"a\":1}";
    request.body = json;
    request.bodyLength = sizeof(json) - 1;
    CHECK(Net::ValidRequest(request));
    request.bodyLength = 0;  // an empty body is a body
    CHECK(Net::ValidRequest(request));
    request.body = nullptr;
    request.bodyLength = 5;  // a length without the bytes
    CHECK(!Net::ValidRequest(request));
    request.bodyLength = 0;

    // A body from a file: a descriptor and a length, both or neither.
    request.bodyFd = 3;
    request.bodyFileLength = 50u * 1024 * 1024;
    CHECK(Net::ValidRequest(request));
    request.bodyFileLength = 0;
    CHECK(!Net::ValidRequest(request));
    request.bodyFd = -1;
    request.bodyFileLength = 10;
    CHECK(!Net::ValidRequest(request));
    // Not both ways.
    request.bodyFd = 3;
    request.body = json;
    request.bodyLength = 3;
    CHECK(!Net::ValidRequest(request));
    // esp_http_client counts the body in an int.
    request.body = nullptr;
    request.bodyLength = 0;
    request.bodyFileLength = 0x7FFFFFFFull;
    CHECK(Net::ValidRequest(request));
    request.bodyFileLength = 0x80000000ull;
    CHECK(!Net::ValidRequest(request));
}

Net::LauncherSettings SavedNetworks(std::initializer_list<const char*> ssids)
{
    Net::LauncherSettings settings;
    for (const char* ssid : ssids) std::strcpy(settings.networks[settings.networkCount++].ssid, ssid);
    return settings;
}

Net::ConnectOrder::Seen SeenAp(const char* ssid, int8_t rssi, uint8_t channel)
{
    Net::ConnectOrder::Seen seen;
    std::strcpy(seen.ssid, ssid);
    seen.rssi = rssi;
    seen.channel = channel;
    return seen;
}

void ConnectOrderTests()
{
    namespace Order = Net::ConnectOrder;
    Order::Attempt attempt;
    Order::Attempt out[Net::kMaxSavedNetworks];

    // Nothing saved: nothing to try, no scan.
    Net::LauncherSettings none;
    CHECK(!Order::LastNetwork(none, attempt));
    CHECK(!Order::WorthScanning(none, false));

    // The last network first, on its channel; it is left out after the scan.
    auto settings = SavedNetworks({"Office", "Home", "Phone"});
    std::strcpy(settings.lastSsid, "Home");
    settings.lastChannel = 11;
    CHECK(Order::LastNetwork(settings, attempt) && attempt.network == 1 && attempt.channel == 11);
    CHECK(Order::WorthScanning(settings, true));
    const Order::Seen seen[] = {SeenAp("Neighbour", -40, 1), SeenAp("Office", -70, 6), SeenAp("Home", -45, 11),
                                SeenAp("Phone", -60, 3), SeenAp("Office", -50, 1), SeenAp("", -30, 9)};
    std::size_t count = Order::AfterScan(settings, seen, 6, 1, out, Net::kMaxSavedNetworks);
    // Office's strongest access point (-50 on channel 1) beats Phone (-60); Home was tried already;
    // a hidden network and an unsaved one are not tried.
    CHECK(count == 2);
    CHECK(out[0].network == 0 && out[0].channel == 1);
    CHECK(out[1].network == 2 && out[1].channel == 3);
    // Without a last network, Home too, strongest first.
    count = Order::AfterScan(settings, seen, 6, -1, out, Net::kMaxSavedNetworks);
    CHECK(count == 3 && out[0].network == 1 && out[1].network == 0 && out[2].network == 2);
    // Capacity is respected.
    CHECK(Order::AfterScan(settings, seen, 6, -1, out, 1) == 1 && out[0].network == 1);
    // Nothing in range.
    const Order::Seen strangers[] = {SeenAp("Cafe", -50, 6)};
    CHECK(Order::AfterScan(settings, strangers, 1, -1, out, Net::kMaxSavedNetworks) == 0);
    CHECK(Order::AfterScan(settings, nullptr, 0, -1, out, Net::kMaxSavedNetworks) == 0);

    // Only the last network is saved: once it has been tried, a scan cannot find another.
    auto single = SavedNetworks({"Home"});
    std::strcpy(single.lastSsid, "Home");
    single.lastChannel = 6;
    CHECK(Order::LastNetwork(single, attempt) && attempt.network == 0);
    CHECK(!Order::WorthScanning(single, true));
    CHECK(Order::WorthScanning(single, false));

    // A last network without a channel, on no valid channel, or no longer saved: scan instead.
    settings.lastChannel = 0;
    CHECK(!Order::LastNetwork(settings, attempt));
    settings.lastChannel = 15;
    CHECK(!Order::LastNetwork(settings, attempt));
    settings.lastChannel = 6;
    std::strcpy(settings.lastSsid, "Gone");
    CHECK(!Order::LastNetwork(settings, attempt));

    // Equal signals keep the saved order; an SSID of the full 32 bytes matches.
    auto equal = SavedNetworks({"B", "A", "0123456789abcdef0123456789abcdef"});
    const Order::Seen tie[] = {SeenAp("A", -60, 1), SeenAp("B", -60, 2), SeenAp("0123456789abcdef0123456789abcdef", -61, 3)};
    count = Order::AfterScan(equal, tie, 3, -1, out, Net::kMaxSavedNetworks);
    CHECK(count == 3 && out[0].network == 0 && out[1].network == 1 && out[2].network == 2 && out[2].channel == 3);
}

// The first 64 bytes of a page made by ESP-IDF 5.3.5's nvs_partition_gen.py (generate, 0x5000) from
// the Launcher's namespaces: l_account (five strings), l_wifi (s_, p_, b_ of one network), launcher
// (wifi_last, wifi_ch). Active, 23 entries written.
const uint8_t kGeneratedPage[64] = {
    0xFE, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x84, 0x2D, 0xBA, 0xB9,
    0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xEA, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

void NvsPageTests()
{
    namespace Page = Net::NvsPage;
    uint8_t page[64];
    std::memcpy(page, kGeneratedPage, sizeof(page));
    Page::Summary summary = Page::Summarize(page, page + 32);
    CHECK(summary.kind == Page::Kind::Data && summary.written == 23);

    // Full: the state is outside the CRC.
    page[0] = 0xFC;
    summary = Page::Summarize(page, page + 32);
    CHECK(summary.kind == Page::Kind::Data && summary.written == 23);
    // Two entries erased (0b00) and one illegal (0b01): only written ones count.
    page[32] = 0xA0;  // entries 0, 1 erased
    page[33] = 0xA9;  // entry 4 illegal
    summary = Page::Summarize(page, page + 32);
    CHECK(summary.kind == Page::Kind::Data && summary.written == 20);
    // Every entry written: 126, the two spare bits of the last word ignored.
    std::memset(page + 32, 0xAA, 32);
    CHECK(Page::Summarize(page, page + 32).written == 126);

    // A page move that was interrupted, a state NVS does not know, a broken header, a newer format.
    std::memcpy(page, kGeneratedPage, sizeof(page));
    page[0] = 0xF8;
    CHECK(Page::Summarize(page, page + 32).kind == Page::Kind::Freeing);
    page[0] = 0xF0;
    CHECK(Page::Summarize(page, page + 32).kind == Page::Kind::Corrupt);
    page[0] = 0x00;
    CHECK(Page::Summarize(page, page + 32).kind == Page::Kind::Corrupt);
    std::memcpy(page, kGeneratedPage, sizeof(page));
    page[4] = 0x01;  // the sequence number is under the CRC
    CHECK(Page::Summarize(page, page + 32).kind == Page::Kind::Corrupt);
    std::memcpy(page, kGeneratedPage, sizeof(page));
    page[8] = 0xFF;  // the first NVS format: still read
    page[28] = 0;
    {
        // Recompute the header CRC for the older version byte (NVS: register 0, inverted).
        uint32_t crc = CardenzaHAL::Net::Internal::Crc32Reflected(0, page + 4, 24) ^ 0xFFFFFFFFu;
        for (int i = 0; i < 4; ++i) page[28 + i] = static_cast<uint8_t>(crc >> (8 * i));
    }
    summary = Page::Summarize(page, page + 32);
    CHECK(summary.kind == Page::Kind::Data && summary.written == 23);
    page[8] = 0xFD;
    {
        uint32_t crc = CardenzaHAL::Net::Internal::Crc32Reflected(0, page + 4, 24) ^ 0xFFFFFFFFu;
        for (int i = 0; i < 4; ++i) page[28 + i] = static_cast<uint8_t>(crc >> (8 * i));
    }
    CHECK(Page::Summarize(page, page + 32).kind == Page::Kind::Newer);

    // Erased.
    std::memset(page, 0xFF, sizeof(page));
    summary = Page::Summarize(page, page + 32);
    CHECK(summary.kind == Page::Kind::Empty && summary.written == 0);
}

}  // namespace

int main()
{
    Crc32Tests();
    Base64Tests();
    DecryptTests();
    AccountTests();
    UrlTests();
    HeaderTests();
    RequestTests();
    ConnectOrderTests();
    NvsPageTests();
    std::cout << "net ok\n";
    return 0;
}
