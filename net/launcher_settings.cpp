// The Launcher's way of storing saved networks, undone: CRC-32 keys, base64 and AES-128-CBC with
// PKCS#7, as 203-launcher src/settings.cpp and src/wifi_crypto.cpp write them. Pure C++ (no ESP-IDF,
// no mbedTLS): the host tests compile it too. The AES below is the FIPS-197 inverse cipher, written
// for this file; only decryption is needed.
#include "net/launcher_settings.h"

#include <cstdio>
#include <cstring>

#include "net/internal/crc32.h"

namespace CardenzaHAL::Net {

namespace {

// Overwrites `size` bytes in a way the compiler cannot drop as a dead store.
void Wipe(void* data, std::size_t size)
{
    volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) bytes[i] = 0;
}

// --- AES-128 (FIPS-197), decryption only -------------------------------------------------------------

constexpr uint8_t kSbox[256] = {
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16,
};

constexpr uint8_t kInvSbox[256] = {
    0x52, 0x09, 0x6A, 0xD5, 0x30, 0x36, 0xA5, 0x38, 0xBF, 0x40, 0xA3, 0x9E, 0x81, 0xF3, 0xD7, 0xFB,
    0x7C, 0xE3, 0x39, 0x82, 0x9B, 0x2F, 0xFF, 0x87, 0x34, 0x8E, 0x43, 0x44, 0xC4, 0xDE, 0xE9, 0xCB,
    0x54, 0x7B, 0x94, 0x32, 0xA6, 0xC2, 0x23, 0x3D, 0xEE, 0x4C, 0x95, 0x0B, 0x42, 0xFA, 0xC3, 0x4E,
    0x08, 0x2E, 0xA1, 0x66, 0x28, 0xD9, 0x24, 0xB2, 0x76, 0x5B, 0xA2, 0x49, 0x6D, 0x8B, 0xD1, 0x25,
    0x72, 0xF8, 0xF6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xD4, 0xA4, 0x5C, 0xCC, 0x5D, 0x65, 0xB6, 0x92,
    0x6C, 0x70, 0x48, 0x50, 0xFD, 0xED, 0xB9, 0xDA, 0x5E, 0x15, 0x46, 0x57, 0xA7, 0x8D, 0x9D, 0x84,
    0x90, 0xD8, 0xAB, 0x00, 0x8C, 0xBC, 0xD3, 0x0A, 0xF7, 0xE4, 0x58, 0x05, 0xB8, 0xB3, 0x45, 0x06,
    0xD0, 0x2C, 0x1E, 0x8F, 0xCA, 0x3F, 0x0F, 0x02, 0xC1, 0xAF, 0xBD, 0x03, 0x01, 0x13, 0x8A, 0x6B,
    0x3A, 0x91, 0x11, 0x41, 0x4F, 0x67, 0xDC, 0xEA, 0x97, 0xF2, 0xCF, 0xCE, 0xF0, 0xB4, 0xE6, 0x73,
    0x96, 0xAC, 0x74, 0x22, 0xE7, 0xAD, 0x35, 0x85, 0xE2, 0xF9, 0x37, 0xE8, 0x1C, 0x75, 0xDF, 0x6E,
    0x47, 0xF1, 0x1A, 0x71, 0x1D, 0x29, 0xC5, 0x89, 0x6F, 0xB7, 0x62, 0x0E, 0xAA, 0x18, 0xBE, 0x1B,
    0xFC, 0x56, 0x3E, 0x4B, 0xC6, 0xD2, 0x79, 0x20, 0x9A, 0xDB, 0xC0, 0xFE, 0x78, 0xCD, 0x5A, 0xF4,
    0x1F, 0xDD, 0xA8, 0x33, 0x88, 0x07, 0xC7, 0x31, 0xB1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xEC, 0x5F,
    0x60, 0x51, 0x7F, 0xA9, 0x19, 0xB5, 0x4A, 0x0D, 0x2D, 0xE5, 0x7A, 0x9F, 0x93, 0xC9, 0x9C, 0xEF,
    0xA0, 0xE0, 0x3B, 0x4D, 0xAE, 0x2A, 0xF5, 0xB0, 0xC8, 0xEB, 0xBB, 0x3C, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2B, 0x04, 0x7E, 0xBA, 0x77, 0xD6, 0x26, 0xE1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0C, 0x7D,
};

constexpr std::size_t kBlock = 16;
constexpr int kRounds = 10;

// Multiplication by x in GF(2^8) with the AES polynomial.
inline uint8_t Times2(uint8_t value)
{
    return static_cast<uint8_t>((value << 1) ^ ((value & 0x80) != 0 ? 0x1B : 0x00));
}

inline uint8_t Multiply(uint8_t value, uint8_t factor)
{
    uint8_t result = 0;
    while (factor != 0)
    {
        if ((factor & 1) != 0) result ^= value;
        value = Times2(value);
        factor >>= 1;
    }
    return result;
}

// The key schedule (FIPS-197 5.2): 11 round keys of 16 bytes, round r at keys[16 r].
void ExpandKey(const uint8_t key[kBlock], uint8_t keys[kBlock * (kRounds + 1)])
{
    std::memcpy(keys, key, kBlock);
    uint8_t roundConstant = 1;
    for (std::size_t i = kBlock; i < kBlock * (kRounds + 1); i += 4)
    {
        uint8_t word[4] = {keys[i - 4], keys[i - 3], keys[i - 2], keys[i - 1]};
        if (i % kBlock == 0)
        {
            const uint8_t first = word[0];
            word[0] = static_cast<uint8_t>(kSbox[word[1]] ^ roundConstant);
            word[1] = kSbox[word[2]];
            word[2] = kSbox[word[3]];
            word[3] = kSbox[first];
            roundConstant = Times2(roundConstant);
        }
        for (std::size_t j = 0; j < 4; ++j) keys[i + j] = static_cast<uint8_t>(keys[i + j - kBlock] ^ word[j]);
    }
}

// The inverse cipher (FIPS-197 5.3) on one block in place. The state is column-major as the bytes
// come: byte 4 c + r is row r of column c.
void DecryptBlock(const uint8_t keys[kBlock * (kRounds + 1)], uint8_t state[kBlock])
{
    for (std::size_t i = 0; i < kBlock; ++i) state[i] ^= keys[kBlock * kRounds + i];
    uint8_t shifted[kBlock];
    for (int round = kRounds - 1; round >= 0; --round)
    {
        // InvShiftRows: row r moves r columns to the right.
        for (std::size_t column = 0; column < 4; ++column)
            for (std::size_t row = 0; row < 4; ++row)
                shifted[4 * ((column + row) % 4) + row] = state[4 * column + row];
        // InvSubBytes and AddRoundKey.
        for (std::size_t i = 0; i < kBlock; ++i)
            state[i] = static_cast<uint8_t>(kInvSbox[shifted[i]] ^ keys[kBlock * round + i]);
        if (round == 0) break;
        // InvMixColumns.
        for (std::size_t column = 0; column < 4; ++column)
        {
            uint8_t* c = state + 4 * column;
            const uint8_t a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
            c[0] = static_cast<uint8_t>(Multiply(a0, 14) ^ Multiply(a1, 11) ^ Multiply(a2, 13) ^ Multiply(a3, 9));
            c[1] = static_cast<uint8_t>(Multiply(a0, 9) ^ Multiply(a1, 14) ^ Multiply(a2, 11) ^ Multiply(a3, 13));
            c[2] = static_cast<uint8_t>(Multiply(a0, 13) ^ Multiply(a1, 9) ^ Multiply(a2, 14) ^ Multiply(a3, 11));
            c[3] = static_cast<uint8_t>(Multiply(a0, 11) ^ Multiply(a1, 13) ^ Multiply(a2, 9) ^ Multiply(a3, 14));
        }
    }
    Wipe(shifted, sizeof(shifted));
}

// --- base64 -------------------------------------------------------------------------------------------

int Base64Value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// The most the Launcher writes: 64 bytes of cipher (63 characters and their padding) are 88 characters.
constexpr std::size_t kMaxCipherText = 88;
constexpr std::size_t kMaxCipherBytes = 64;
constexpr std::size_t kMaxPassword = 63;

}  // namespace

void LauncherSettings::Forget()
{
    for (auto& network : networks) Wipe(network.password, sizeof(network.password));
}

bool ValidBinding(const LauncherAccount& account)
{
    // The Launcher's communityValidToken(token, "ccd_"): the prefix and exactly 64 hex digits.
    if (std::strncmp(account.token, "ccd_", 4) != 0) return false;
    std::size_t digits = 0;
    for (const char* c = account.token + 4; *c != 0; ++c, ++digits)
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'))) return false;
    return digits == 64 && account.api[0] != 0;
}

void CopyUtf8(const char* text, char* out, std::size_t capacity)
{
    if (out == nullptr || capacity == 0) return;
    std::size_t length = text != nullptr ? std::strlen(text) : 0;
    if (length >= capacity)
    {
        length = capacity - 1;
        // Back to the first byte of the character that does not fit (continuation bytes are 10xxxxxx).
        while (length > 0 && (static_cast<uint8_t>(text[length]) & 0xC0) == 0x80) --length;
    }
    if (length > 0) std::memcpy(out, text, length);
    out[length] = 0;
}

namespace LauncherWifi {

uint32_t Crc32(const void* data, std::size_t length)
{
    // The Launcher's crc32(): bitwise, register 0xFFFFFFFF, reflected polynomial 0xEDB88320, inverted.
    return Internal::Crc32Reflected(0xFFFFFFFFu, data, length) ^ 0xFFFFFFFFu;
}

void EntryKey(char prefix, const char* ssid, char key[11])
{
    const uint32_t crc = Crc32(ssid, ssid != nullptr ? std::strlen(ssid) : 0);
    std::snprintf(key, 11, "%c_%08X", prefix, static_cast<unsigned>(crc));
}

bool Base64Decode(const char* text, std::size_t length, uint8_t* out, std::size_t capacity, std::size_t& written)
{
    written = 0;
    if (length == 0) return true;
    if (text == nullptr || length % 4 != 0) return false;
    std::size_t padding = 0;
    if (text[length - 1] == '=') padding = text[length - 2] == '=' ? 2 : 1;
    const std::size_t size = length / 4 * 3 - padding;
    if (size > capacity || out == nullptr) return false;
    for (std::size_t i = 0; i < length; i += 4)
    {
        const bool last = i + 4 == length;
        int values[4];
        for (std::size_t j = 0; j < 4; ++j)
        {
            const bool pad = last && j >= 4 - padding;
            values[j] = pad ? 0 : Base64Value(text[i + j]);
            // '=' only as the padding at the very end; nothing else outside the alphabet.
            if (values[j] < 0 || (pad && text[i + j] != '=')) return false;
        }
        const uint32_t group = static_cast<uint32_t>(values[0]) << 18 | static_cast<uint32_t>(values[1]) << 12 |
                               static_cast<uint32_t>(values[2]) << 6 | static_cast<uint32_t>(values[3]);
        // Canonical only: the bits a padded group drops are zero.
        if (last && ((padding == 1 && (group & 0xFF) != 0) || (padding == 2 && (group & 0xFFFF) != 0))) return false;
        const uint8_t bytes[3] = {static_cast<uint8_t>(group >> 16), static_cast<uint8_t>(group >> 8),
                                  static_cast<uint8_t>(group)};
        const std::size_t count = last ? 3 - padding : 3;
        for (std::size_t j = 0; j < count; ++j) out[written++] = bytes[j];
    }
    return true;
}

bool DecryptPassword(const char* cipher, char* plain, std::size_t capacity)
{
    if (plain == nullptr || capacity == 0) return false;
    plain[0] = 0;
    if (cipher == nullptr) return false;
    const std::size_t textLength = std::strlen(cipher);
    if (textLength == 0) return true;  // an open network
    if (textLength > kMaxCipherText) return false;

    uint8_t buffer[kMaxCipherBytes];
    std::size_t size = 0;
    bool ok = Base64Decode(cipher, textLength, buffer, sizeof(buffer), size) && size != 0 && size % kBlock == 0;

    uint8_t keys[kBlock * (kRounds + 1)];
    ExpandKey(reinterpret_cast<const uint8_t*>(kKey), keys);
    // CBC: each plaintext block is the decrypted block XOR the cipher block before it (the IV first).
    uint8_t previous[kBlock];
    uint8_t current[kBlock];
    std::memcpy(previous, kIv, kBlock);
    for (std::size_t offset = 0; ok && offset < size; offset += kBlock)
    {
        std::memcpy(current, buffer + offset, kBlock);
        DecryptBlock(keys, buffer + offset);
        for (std::size_t i = 0; i < kBlock; ++i) buffer[offset + i] ^= previous[i];
        std::memcpy(previous, current, kBlock);
    }

    // PKCS#7 as the Launcher's wifiPwdDecrypt checks it: 1..16 bytes, fewer than the whole, all equal.
    std::size_t length = 0;
    if (ok)
    {
        const uint8_t padding = buffer[size - 1];
        ok = padding != 0 && padding <= kBlock && padding < size;
        for (std::size_t i = size - (ok ? padding : 0); ok && i < size; ++i) ok = buffer[i] == padding;
        length = ok ? size - padding : 0;
    }
    // A C string cannot carry a zero byte: such a password would come out cut short.
    ok = ok && length <= kMaxPassword && length < capacity && std::memchr(buffer, 0, length) == nullptr;
    if (ok)
    {
        std::memcpy(plain, buffer, length);
        plain[length] = 0;
    }
    Wipe(buffer, sizeof(buffer));
    Wipe(keys, sizeof(keys));
    Wipe(previous, sizeof(previous));
    Wipe(current, sizeof(current));
    return ok;
}

}  // namespace LauncherWifi

}  // namespace CardenzaHAL::Net
