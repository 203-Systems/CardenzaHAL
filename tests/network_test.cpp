// usb/network_logic.h: the USB network's address plan, the two MACs of the link, the host name, and the minimal
// mDNS responder's query matching and answers (RFC 6762).
#include "usb/network_logic.h"

#include <cassert>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

using namespace CardenzaHAL::USB::NetworkLogic;

namespace {

void MacTests()
{
    const uint8_t universal[6] = {0x70, 0xAF, 0x09, 0xBC, 0xEB, 0x33};
    uint8_t host[6];
    HostMac(universal, host);
    assert((host[0] & 0x02) && !(host[0] & 0x01));  // locally administered, unicast
    assert(std::memcmp(host, universal, 6) != 0);
    assert(std::memcmp(host + 1, universal + 1, 5) == 0);  // tied to the chip
    // A device MAC that is already a local one still gets a different host MAC.
    const uint8_t local[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    HostMac(local, host);
    assert(std::memcmp(host, local, 6) != 0 && (host[0] & 0x02) && !(host[0] & 0x01));
    // A multicast bit in the source never reaches the host's MAC.
    const uint8_t odd[6] = {0x71, 0, 0, 0, 0, 1};
    HostMac(odd, host);
    assert(!(host[0] & 0x01));

    char text[13];
    MacString(universal, text);
    assert(std::string(text) == "70AF09BCEB33");
}

void HostNameTests()
{
    char name[33];
    HostName("Cardenza", name);
    assert(std::string(name) == "cardenza");
    HostName("my synth_2!", name);
    assert(std::string(name) == "mysynth2");
    HostName("", name);
    assert(std::string(name) == "cardenza");
    HostName(nullptr, name);
    assert(std::string(name) == "cardenza");
    HostName("-bad", name);
    assert(std::string(name) == "cardenza");
    HostName("studio-a", name);
    assert(std::string(name) == "studio-a");
    HostName("abcdefghijklmnopqrstuvwxyz0123456789", name);
    assert(std::strlen(name) == 32);
}

// A query: header, then each question as (labels, type, class); `pointerTo` makes a question's name a pointer.
struct Question {
    std::vector<std::string> labels;
    uint16_t type;
    uint16_t klass;
    int pointerTo = -1;  // offset of an earlier name
};

std::vector<uint8_t> MakeQuery(uint16_t id, uint16_t flags, std::initializer_list<Question> questions, std::vector<std::size_t>* offsets = nullptr)
{
    std::vector<uint8_t> packet = {static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id), static_cast<uint8_t>(flags >> 8),
                                   static_cast<uint8_t>(flags), 0, static_cast<uint8_t>(questions.size()), 0, 0, 0, 0, 0, 0};
    for (const Question& question : questions)
    {
        if (offsets) offsets->push_back(packet.size());
        if (question.pointerTo >= 0)
        {
            packet.push_back(static_cast<uint8_t>(0xC0 | (question.pointerTo >> 8)));
            packet.push_back(static_cast<uint8_t>(question.pointerTo));
        }
        else
        {
            for (const std::string& label : question.labels)
            {
                packet.push_back(static_cast<uint8_t>(label.size()));
                packet.insert(packet.end(), label.begin(), label.end());
            }
            packet.push_back(0);
        }
        packet.push_back(static_cast<uint8_t>(question.type >> 8));
        packet.push_back(static_cast<uint8_t>(question.type));
        packet.push_back(static_cast<uint8_t>(question.klass >> 8));
        packet.push_back(static_cast<uint8_t>(question.klass));
    }
    return packet;
}

void QueryTests()
{
    Query query;
    auto packet = MakeQuery(0x1234, 0, {{{"cardenza", "local"}, kTypeA, kClassIn}});
    assert(ParseQuery(packet.data(), packet.size(), "cardenza", query));
    assert(query.asksForUs && !query.unicast && query.id == 0x1234);

    // Case does not matter; ANY counts; the QU bit is seen.
    packet = MakeQuery(1, 0, {{{"CarDenza", "LOCAL"}, kTypeAny, static_cast<uint16_t>(0x8000 | kClassIn)}});
    assert(ParseQuery(packet.data(), packet.size(), "cardenza", query) && query.unicast);

    // Another name, a longer one, a shorter one, AAAA only, another class: not for us.
    for (const auto& other : {MakeQuery(1, 0, {{{"cardenza2", "local"}, kTypeA, kClassIn}}),
                              MakeQuery(1, 0, {{{"cardenza", "local", "x"}, kTypeA, kClassIn}}),
                              MakeQuery(1, 0, {{{"cardenza"}, kTypeA, kClassIn}}),
                              MakeQuery(1, 0, {{{"card", "local"}, kTypeA, kClassIn}}),
                              MakeQuery(1, 0, {{{"cardenza", "local"}, 28, kClassIn}}),
                              MakeQuery(1, 0, {{{"cardenza", "local"}, kTypeA, 3}})})
        assert(!ParseQuery(other.data(), other.size(), "cardenza", query));

    // The second question, a compressed pointer to the first's name ("_http._tcp.local" style packets mix them).
    std::vector<std::size_t> offsets;
    packet = MakeQuery(7, 0, {{{"printer", "local"}, kTypeA, kClassIn}, {{"cardenza", "local"}, 28, kClassIn}}, &offsets);
    const std::size_t nameAt = offsets[1];
    auto withPointer = MakeQuery(7, 0, {{{"printer", "local"}, kTypeA, kClassIn},
                                        {{"cardenza", "local"}, 28, kClassIn},
                                        {{}, kTypeA, kClassIn, static_cast<int>(nameAt)}});
    assert(ParseQuery(withPointer.data(), withPointer.size(), "cardenza", query));

    // A response, an opcode other than a query: ignored.
    packet = MakeQuery(1, 0x8400, {{{"cardenza", "local"}, kTypeA, kClassIn}});
    assert(!ParseQuery(packet.data(), packet.size(), "cardenza", query));
    packet = MakeQuery(1, 0x2800, {{{"cardenza", "local"}, kTypeA, kClassIn}});
    assert(!ParseQuery(packet.data(), packet.size(), "cardenza", query));

    // Malformed: truncated anywhere, a pointer loop, a label running past the end.
    packet = MakeQuery(1, 0, {{{"cardenza", "local"}, kTypeA, kClassIn}});
    for (std::size_t length = 0; length < packet.size(); ++length) assert(!ParseQuery(packet.data(), length, "cardenza", query));
    std::vector<uint8_t> loop = {0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 12, 0, 1, 0, 1};
    assert(!ParseQuery(loop.data(), loop.size(), "cardenza", query));
    std::vector<uint8_t> runaway = {0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 40, 'c', 'a'};
    assert(!ParseQuery(runaway.data(), runaway.size(), "cardenza", query));
}

uint16_t At16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }

void AnswerTests()
{
    const uint8_t address[4] = {192, 168, 203, 1};
    uint8_t out[128];
    // Multicast: id 0, no question, cache-flush, TTL 120.
    std::size_t length = BuildAnswer("cardenza", address, false, 0x1234, out, sizeof(out));
    const uint8_t name[] = {8, 'c', 'a', 'r', 'd', 'e', 'n', 'z', 'a', 5, 'l', 'o', 'c', 'a', 'l', 0};
    assert(length == 12 + sizeof(name) + 10 + 4);
    assert(At16(out) == 0 && At16(out + 2) == 0x8400 && At16(out + 4) == 0 && At16(out + 6) == 1);
    assert(std::memcmp(out + 12, name, sizeof(name)) == 0);
    const uint8_t* record = out + 12 + sizeof(name);
    assert(At16(record) == kTypeA && At16(record + 2) == 0x8001);
    assert(At16(record + 4) == 0 && At16(record + 6) == kTtl && At16(record + 8) == 4);
    assert(std::memcmp(record + 10, address, 4) == 0);

    // Legacy unicast: the query's id, its question, a pointer to its name, no cache-flush, TTL 10.
    length = BuildAnswer("cardenza", address, true, 0x1234, out, sizeof(out));
    assert(At16(out) == 0x1234 && At16(out + 4) == 1 && At16(out + 6) == 1);
    assert(std::memcmp(out + 12, name, sizeof(name)) == 0);
    const uint8_t* question = out + 12 + sizeof(name);
    assert(At16(question) == kTypeA && At16(question + 2) == kClassIn);
    record = question + 4;
    assert(At16(record) == 0xC00C && At16(record + 2) == kTypeA && At16(record + 4) == kClassIn);
    assert(At16(record + 8) == kLegacyTtl && At16(record + 10) == 4 && std::memcmp(record + 12, address, 4) == 0);
    assert(length == static_cast<std::size_t>(record + 16 - out));

    // Our own answer is a response: a query parser takes no notice of it.
    Query query;
    assert(!ParseQuery(out, length, "cardenza", query));

    // Too small, by a byte or more: nothing written; exactly enough: the whole answer.
    for (std::size_t capacity = 0; capacity < 42; ++capacity) assert(BuildAnswer("cardenza", address, false, 0, out, capacity) == 0);
    assert(BuildAnswer("cardenza", address, false, 0, out, 42) == 42);
    for (std::size_t capacity = 0; capacity < 48; ++capacity) assert(BuildAnswer("cardenza", address, true, 1, out, capacity) == 0);
    assert(BuildAnswer("cardenza", address, true, 1, out, 48) == 48);
}

}  // namespace

int main()
{
    MacTests();
    HostNameTests();
    QueryTests();
    AnswerTests();
    std::cout << "network tests passed\n";
    return 0;
}
