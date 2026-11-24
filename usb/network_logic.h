#pragma once

#include <cstddef>
#include <cstdint>

// The USB network's pure parts (usb/network.h): the addresses of the device mode, the two MACs of the link, and
// the minimal mDNS responder that answers "<name>.local" on the USB link. No ESP-IDF and no lwIP here:
// usb/network.cpp uses it on the board, tests/network_test.cpp on the host.
namespace CardenzaHAL::USB::NetworkLogic {

// ---- The device mode's addresses ----------------------------------------------------------------------------
//
// The board answers at 192.168.203.1 on a /24 of its own and hands the computer an address from its DHCP server
// (192.168.203.2 onwards). 192.168.203.0/24 is a private range home and office networks rarely use (they keep to
// 192.168.0.x / 1.x and 10.x). No router and no DNS server are offered: the computer never sends its internet
// traffic through the board and keeps its own routes and names.
inline constexpr uint8_t kDeviceAddress[4] = {192, 168, 203, 1};
inline constexpr uint8_t kNetmask[4] = {255, 255, 255, 0};
inline constexpr uint32_t kLeaseMinutes = 120;

// ---- MACs ---------------------------------------------------------------------------------------------------
//
// Two ends, two MACs: the board's lwIP side takes the chip's own Ethernet MAC (universally administered, unique);
// the computer's NIC takes the one the NCM descriptor names, derived from it as a locally administered unicast
// address (bit 1 of the first byte set, bit 0 clear), so it never equals the board's or another chip's.
inline void HostMac(const uint8_t device[6], uint8_t out[6])
{
    for (int index = 0; index < 6; ++index) out[index] = device[index];
    out[0] = static_cast<uint8_t>((out[0] | 0x02) & ~0x01);
    bool same = true;
    for (int index = 0; index < 6; ++index) same = same && out[index] == device[index];
    if (same) out[5] ^= 0x01;  // the device's was already a local one: still a different address
}

// "02AF09BCEB33": the NCM descriptor's iMACAddress (12 upper-case hex digits). `out` holds 13 bytes.
inline void MacString(const uint8_t mac[6], char out[13])
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    for (int index = 0; index < 6; ++index)
    {
        out[2 * index] = kDigits[mac[index] >> 4];
        out[2 * index + 1] = kDigits[mac[index] & 15];
    }
    out[12] = '\0';
}

// A host name for mDNS and DHCP: lower case letters, digits and '-', 1 to 32 characters, else "cardenza".
inline void HostName(const char* asked, char out[33])
{
    std::size_t length = 0;
    for (const char* c = asked; c != nullptr && *c != '\0' && length < 32; ++c)
    {
        char letter = *c;
        if (letter >= 'A' && letter <= 'Z') letter = static_cast<char>(letter - 'A' + 'a');
        const bool fine = (letter >= 'a' && letter <= 'z') || (letter >= '0' && letter <= '9') || letter == '-';
        if (fine) out[length++] = letter;
    }
    if (length == 0 || out[0] == '-' || out[length - 1] == '-')
    {
        const char kDefault[] = "cardenza";
        for (length = 0; kDefault[length] != '\0'; ++length) out[length] = kDefault[length];
    }
    out[length] = '\0';
}

// ---- mDNS (RFC 6762), just enough to be found as <name>.local --------------------------------------------------

inline constexpr uint16_t kMdnsPort = 5353;
inline constexpr uint8_t kMdnsGroup[4] = {224, 0, 0, 251};
inline constexpr uint16_t kTypeA = 1;
inline constexpr uint16_t kTypeAny = 255;
inline constexpr uint16_t kClassIn = 1;
inline constexpr uint32_t kTtl = 120;
inline constexpr uint32_t kLegacyTtl = 10;  // RFC 6762 6.7: at most 10 s in an answer to a legacy (one-shot) query

struct Query {
    uint16_t id = 0;
    bool asksForUs = false;   // a question for <name>.local, type A or ANY, class IN
    bool unicast = false;     // its QU bit: the querier would take a unicast answer
};

namespace detail {

inline uint16_t Read16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }

inline char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// Compares the name at `at` (labels, maybe compressed) with "<name>.local", case-insensitively, and moves `at`
// past it (past the first pointer when there is one). False when the packet is malformed.
inline bool NameIs(const uint8_t* packet, std::size_t length, std::size_t& at, const char* name, bool& equal)
{
    const char* labels[2] = {name, "local"};
    std::size_t label = 0;
    equal = true;
    std::size_t cursor = at;
    bool jumped = false;
    for (int hops = 0; hops < 16; ++hops)
    {
        if (cursor >= length) return false;
        const uint8_t size = packet[cursor];
        if ((size & 0xC0) == 0xC0)
        {
            if (cursor + 1 >= length) return false;
            if (!jumped) at = cursor + 2;
            jumped = true;
            cursor = static_cast<std::size_t>((size & 0x3F) << 8 | packet[cursor + 1]);
            continue;
        }
        if (size & 0xC0) return false;
        if (size == 0)
        {
            if (!jumped) at = cursor + 1;
            equal = equal && label == 2;
            return true;
        }
        if (cursor + 1 + size > length) return false;
        if (label < 2)
        {
            const char* want = labels[label];
            std::size_t index = 0;
            for (; index < size; ++index)
                if (want[index] == '\0' || Lower(static_cast<char>(packet[cursor + 1 + index])) != Lower(want[index])) break;
            if (index != size || want[size] != '\0') equal = false;
        }
        else
            equal = false;
        ++label;
        cursor += 1 + size;
    }
    return false;  // a loop of pointers
}

}  // namespace detail

// A packet that came to port 5353: is it a query that asks for <name>.local? Answers, malformed packets and
// questions for others are not.
inline bool ParseQuery(const uint8_t* packet, std::size_t length, const char* name, Query& out)
{
    out = Query{};
    if (length < 12) return false;
    out.id = detail::Read16(packet);
    const uint16_t flags = detail::Read16(packet + 2);
    if (flags & 0x8000) return false;          // a response
    if ((flags >> 11) & 0x0F) return false;    // opcode: standard queries only
    const uint16_t questions = detail::Read16(packet + 4);
    std::size_t at = 12;
    for (uint16_t index = 0; index < questions; ++index)
    {
        bool equal = false;
        if (!detail::NameIs(packet, length, at, name, equal)) return false;
        if (at + 4 > length) return false;
        const uint16_t type = detail::Read16(packet + at);
        const uint16_t klass = detail::Read16(packet + at + 2);
        at += 4;
        if (equal && (type == kTypeA || type == kTypeAny) && (klass & 0x7FFF) == kClassIn)
        {
            out.asksForUs = true;
            out.unicast = out.unicast || (klass & 0x8000) != 0;
        }
    }
    return out.asksForUs;
}

namespace detail {

inline std::size_t PutName(const char* name, uint8_t* out, std::size_t at, std::size_t capacity)
{
    const char* labels[2] = {name, "local"};
    for (const char* label : labels)
    {
        std::size_t size = 0;
        while (label[size] != '\0') ++size;
        if (size == 0 || size > 63 || at + 1 + size >= capacity) return 0;
        out[at++] = static_cast<uint8_t>(size);
        for (std::size_t index = 0; index < size; ++index) out[at++] = static_cast<uint8_t>(label[index]);
    }
    out[at++] = 0;
    return at;
}

inline std::size_t Put16(uint16_t value, uint8_t* out, std::size_t at)
{
    out[at] = static_cast<uint8_t>(value >> 8);
    out[at + 1] = static_cast<uint8_t>(value);
    return at + 2;
}

}  // namespace detail

// The answer: "<name>.local IN A <address>". A multicast answer (an announcement, or a reply on 5353) has id 0,
// no question and the cache-flush bit (the name is ours alone on the link); a legacy unicast reply (a querier
// that did not send from port 5353) repeats its id and question, without cache-flush, with a TTL of 10 s.
// Returns the length written, 0 when it does not fit.
inline std::size_t BuildAnswer(const char* name, const uint8_t address[4], bool legacy, uint16_t id, uint8_t* out,
                               std::size_t capacity)
{
    if (capacity < 12) return 0;
    std::size_t at = 0;
    at = detail::Put16(legacy ? id : 0, out, at);
    at = detail::Put16(0x8400, out, at);           // a response, authoritative
    at = detail::Put16(legacy ? 1 : 0, out, at);   // questions
    at = detail::Put16(1, out, at);                // answers
    at = detail::Put16(0, out, at);
    at = detail::Put16(0, out, at);
    std::size_t nameAt = at;
    if (legacy)
    {
        at = detail::PutName(name, out, at, capacity);
        if (at == 0 || at + 4 > capacity) return 0;
        at = detail::Put16(kTypeA, out, at);
        at = detail::Put16(kClassIn, out, at);
        // The answer names the question's name by a pointer to it.
        if (at + 2 > capacity) return 0;
        at = detail::Put16(static_cast<uint16_t>(0xC000 | nameAt), out, at);
    }
    else
    {
        at = detail::PutName(name, out, at, capacity);
        if (at == 0) return 0;
    }
    if (at + 14 > capacity) return 0;
    at = detail::Put16(kTypeA, out, at);
    at = detail::Put16(legacy ? kClassIn : static_cast<uint16_t>(0x8000 | kClassIn), out, at);
    const uint32_t ttl = legacy ? kLegacyTtl : kTtl;
    at = detail::Put16(static_cast<uint16_t>(ttl >> 16), out, at);
    at = detail::Put16(static_cast<uint16_t>(ttl), out, at);
    at = detail::Put16(4, out, at);
    for (int index = 0; index < 4; ++index) out[at++] = address[index];
    return at;
}

}  // namespace CardenzaHAL::USB::NetworkLogic
