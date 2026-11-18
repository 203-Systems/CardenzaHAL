#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_err.h>

#include "net/http_request.h"
#include "net/launcher_settings.h"

// The network on the device, for a firmware that reaches the internet now and then (share a file,
// check something) over the Wi-Fi and the account the 203 Launcher set up. Nothing runs until asked:
// Connect brings Wi-Fi up, Send makes HTTP(S) requests, Disconnect takes everything down again and
// gives the memory back. Nothing is ever written to the flash: the Launcher's settings are read
// read-only and Wi-Fi keeps its state in RAM.
//
// Calls block (seconds: a connection, a TLS handshake, an upload): make them from a task of their own,
// never from the UI or the audio task, one task at a time. Its stack wants 6-8 KB (TLS;
// examples/launcher_net logs what it used).
//
// Memory: linking Net (calling any of it) adds about 36 KB of static internal RAM (Wi-Fi, PHY and lwIP
// data, and 13 KB of their code that must stay in IRAM) and about 610 KB of flash; a firmware that
// never calls it does not change. Wi-Fi, lwIP and the event loop take some 40-60 KB more of internal
// RAM while connected (Wi-Fi's buffers must be internal unless ESP-IDF puts PSRAM in malloc); TLS takes
// another 30-50 KB, in PSRAM when the firmware sets CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC (the HAL then
// allocates mbedTLS's memory in PSRAM when the heap has some, else internal). README.md lists the
// sdkconfig a firmware needs.
namespace CardenzaHAL::Net {

    enum class SettingsResult : uint8_t {
        Ok,           // read (the account may still be unbound, the list empty)
        NoPartition,  // no NVS partition named "nvs" (not a Launcher's table)
        Unreadable,   // the partition is not in a state that can be read without writing it
    };
    // The Launcher's account binding, saved networks and last network, read without writing anything:
    // the NVS partition is opened through a read-only copy of its partition entry (every write or erase
    // NVS would try is refused by esp_partition), read, and closed again; a partition that would need a
    // repair to open (an interrupted write) is Unreadable, never repaired. When the firmware has opened
    // "nvs" itself, it is read through that and left open. Saved networks whose password does not
    // decrypt are left out. Does not need Wi-Fi; `settings` is cleared first.
    SettingsResult ReadLauncherSettings(LauncherSettings& settings);

    enum class ConnectStatus : uint8_t {
        Connected,
        NoNetworks,   // the Launcher has no saved network
        NotFound,     // none of them in range
        AuthFailed,   // a network refused the password
        Failed,       // no IP address, or Wi-Fi did not start
        NoMemory,
        Cancelled,
    };
    struct ConnectReport {
        char ssid[33] = {};      // the network joined, or the last one tried
        int8_t rssi = 0;
        uint32_t millis = 0;     // how long it took
        esp_err_t error = ESP_OK;
    };
    // Return true to stop what is running (polled every 100 ms or so, and between upload blocks).
    using CancelCheck = bool (*)(void* context);

    // Wi-Fi up as a station on the Launcher's networks: the one it joined last first (on its channel,
    // no scan), then the others it saved that a scan finds, strongest first (net/connect_order.h).
    // Waits for an IP address. Brings up everything it needs (netif, the default event loop unless the
    // firmware has one, Wi-Fi with RAM storage) and takes it down again when it fails. Refused (Failed)
    // when the firmware runs Wi-Fi itself. A connection that drops later is not rejoined: Connected()
    // turns false, and Connect starts over.
    ConnectStatus Connect(const LauncherSettings& settings, ConnectReport& report, uint32_t timeoutMs = 20000,
                          CancelCheck cancelled = nullptr, void* context = nullptr);
    bool Connected();
    // Wi-Fi off and deinitialized, its netif and the default event loop gone: what Connect took is
    // back (ESP-IDF cannot take lwIP's own task down; it stays, idle, after the first Connect).
    void Disconnect();

    enum class SendStatus : uint8_t {
        Ok,              // a response came (any HTTP status: look at Response::status)
        InvalidRequest,  // ValidRequest refused it
        NotConnected,
        ConnectFailed,   // DNS, TCP or TLS (a certificate the bundle does not trust)
        SendFailed,      // the connection broke while sending (or a body file could not be read)
        ReadFailed,      // ... while reading the response
        Cancelled,
        NoMemory,
    };
    struct Response {
        int status = 0;            // the HTTP status (0: none came)
        char* body = nullptr;      // the response body goes here (zero-terminated), up to capacity - 1
        std::size_t capacity = 0;
        std::size_t length = 0;
        bool truncated = false;    // the body was longer
        esp_err_t error = ESP_OK;  // what failed below HTTP
    };
    // Sent so far and the whole body; return false to cancel.
    using Progress = bool (*)(void* context, uint64_t sent, uint64_t total);

    // One request: a fresh connection (https verified with ESP-IDF's certificate bundle), the body from
    // memory or streamed from a file descriptor in kUploadBlockBytes blocks (reported through
    // `progress` after each block, and checked for `cancelled`), then the response body into `response`.
    // Redirects are not followed (a 3xx comes back as it is). The log names the method, the host, the
    // status, the bytes and the time, never the path, the query or a header's value.
    SendStatus Send(const Request& request, Response& response, Progress progress = nullptr,
                    CancelCheck cancelled = nullptr, void* context = nullptr);

    // The running firmware's image digest as the community knows a build by it: what
    // esp_partition_get_sha256 gives for the running app partition (the image's appended SHA-256), as
    // 64 lower-case hex digits and a terminating zero. False when it cannot be read (an image built
    // without the appended digest). Read from the image's headers (a dozen small flash reads, the first
    // call only; the result is kept): the bootloader checked the image when it started it.
    bool RunningImageDigest(char hex[65]);

}  // namespace CardenzaHAL::Net
