#pragma once

#include <cstddef>
#include <cstdint>

// The 203 Launcher's settings a firmware on the device may use, as plain data, and how the Launcher
// stores them. Pure (no ESP-IDF): host tests and simulators include it. Net::ReadLauncherSettings
// (net/net.h) fills it from the shared NVS on the device, read only.
//
// The Launcher keeps, in the NVS partition every firmware on the device shares:
//   l_account  token (the device token, "ccd_" + 64 hex), api (the community's API base URL), user_id,
//              user_name, device_id (the chip's 12-hex MAC): the community account this device is
//              bound to (none: not bound).
//   l_wifi     per saved network, keyed by the CRC-32 of its SSID in upper-case hex:
//              s_<CRC> the SSID, p_<CRC> its password (AES-128-CBC with PKCS#7 padding under a key and
//              IV every Launcher shares, then base64; an open network has an empty one), b_<CRC> = 1.
//   launcher   wifi_last (the SSID it joined last), wifi_ch (its channel) and remote_install
//              (Settings > Account > Receive firmware: 0 off, else or missing on).
// Firmware only reads them: the Launcher owns these namespaces.
//
// None of this is secret from firmware on the device. The passwords are only obfuscated: the key and IV
// are the same on every Launcher and written below, so any firmware on the board (and anyone who reads
// the flash) can recover the Wi-Fi passwords, and read the device token as it is. The token is a least-
// privilege credential for that reason (the community's "device" kind). Real protection would need a
// per-device key (an eFuse HMAC key, or flash encryption with NVS encryption), which the Launcher does
// not use today.
namespace CardenzaHAL::Net {

    // The community account the Launcher bound this device to.
    struct LauncherAccount {
        char token[80] = {};     // the device token: "Authorization: Bearer <token>"; empty: not bound
        char api[128] = {};      // e.g. "https://community.203.io/api", no trailing slash needed
        char userId[16] = {};
        char userName[64] = {};  // cut at a character boundary when longer (the Launcher keeps 64 bytes)
        char deviceId[16] = {};
        bool Bound() const { return token[0] != 0 && api[0] != 0; }
    };

    // What the Launcher's own loadAccount accepts as a binding: a device token ("ccd_" and 64
    // lower-case hex digits) and an API URL. ReadLauncherSettings leaves anything else unbound.
    bool ValidBinding(const LauncherAccount& account);

    // `text` into `out` (at most capacity - 1 bytes and a zero), cut before a UTF-8 character that would
    // not fit whole.
    void CopyUtf8(const char* text, char* out, std::size_t capacity);

    // A network the Launcher saved, password decrypted (empty for an open network).
    struct WifiNetwork {
        char ssid[33] = {};
        char password[64] = {};
    };

    inline constexpr std::size_t kMaxSavedNetworks = 16;

    struct LauncherSettings {
        LauncherAccount account;
        WifiNetwork networks[kMaxSavedNetworks];
        std::size_t networkCount = 0;
        char lastSsid[33] = {};      // the network the Launcher joined last ("": none)
        uint8_t lastChannel = 0;     // its channel (0: unknown)
        // Settings > Account > Receive firmware in the Launcher: off, nothing sent from the community is
        // taken (the presence service does not ask; net/presence.h).
        bool receiveFirmware = true;
        // Clears the passwords (call when done with them).
        void Forget();
    };

    // The Launcher's way of storing a saved network (203-launcher src/settings.cpp, src/wifi_crypto.cpp).
    namespace LauncherWifi {
        // The Launcher's AES-128 key and IV for saved passwords: the same on every device, so this is
        // obfuscation, not encryption (see above).
        inline constexpr char kKey[17] = "LauncherWifi2025";
        inline constexpr uint8_t kIv[16] = {0x4C, 0x61, 0x75, 0x6E, 0x63, 0x68, 0x65, 0x72,
                                            0x57, 0x69, 0x66, 0x69, 0x4B, 0x65, 0x79, 0x21};  // "LauncherWifiKey!"
        // CRC-32 (IEEE, reflected, as zlib) of the SSID's bytes.
        uint32_t Crc32(const void* data, std::size_t length);
        // The NVS key of an entry: `prefix` ('s', 'p' or 'b') + '_' + 8 upper-case hex digits of
        // Crc32(ssid). `key` gets 10 characters and a terminating zero.
        void EntryKey(char prefix, const char* ssid, char key[11]);
        // Standard base64 (RFC 4648, with padding). False for anything else or when it does not fit.
        bool Base64Decode(const char* text, std::size_t length, uint8_t* out, std::size_t capacity, std::size_t& written);
        // A stored password back to text: base64, AES-128-CBC with kKey and kIv, PKCS#7, checked as the
        // Launcher's wifiPwdDecrypt checks it. An empty `cipher` is an open network (empty password).
        // False when it is not what the Launcher wrote (bad base64, over 88 characters, a length that
        // is not whole blocks, bad padding, more than 63 characters), when it holds a zero byte (the
        // Launcher's String would keep it, a C string cannot) or does not fit `capacity`; `plain` is
        // then empty. Never returns a half-decoded password. The AES is the HAL's own (no mbedTLS).
        bool DecryptPassword(const char* cipher, char* plain, std::size_t capacity);
    }

}  // namespace CardenzaHAL::Net
