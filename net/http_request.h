#pragma once

#include <cstddef>
#include <cstdint>

// An HTTP(S) request as Net::Send (net/net.h) takes it, and the checks it makes before anything goes
// on the air. Pure (no ESP-IDF): host tests and simulators include it.
namespace CardenzaHAL::Net {

    struct Header {
        const char* name = nullptr;   // a token (RFC 9110): letters, digits and !#$%&'*+-.^_`|~
        const char* value = nullptr;  // printable text; never CR or LF (a header from a server's answer
                                      // is passed on only when it is clean)
    };

    struct Request {
        const char* method = "GET";   // GET, POST, PUT, DELETE, HEAD or PATCH
        const char* url = nullptr;    // http:// or https:// (https is verified against the certificate bundle)
        const Header* headers = nullptr;
        std::size_t headerCount = 0;
        // The body: in memory, or read from a file descriptor (from its current offset, `bodyFileLength`
        // bytes, never through a FILE: blocks of kUploadBlockBytes go through read()). Not both. With
        // neither, a POST or PUT sends an empty body (Content-Length: 0).
        const void* body = nullptr;
        std::size_t bodyLength = 0;
        int bodyFd = -1;
        uint64_t bodyFileLength = 0;
        uint32_t timeoutMs = 20000;   // per network operation (connect, each write, each read)
    };

    // What a URL is made of. Pointers point into the URL; lengths say how far.
    struct UrlParts {
        bool https = false;
        const char* host = nullptr;
        std::size_t hostLength = 0;
        uint16_t port = 0;            // the scheme's default when the URL names none
        const char* path = nullptr;   // from the first '/' (with the query); "/" when the URL has none
        std::size_t pathLength = 0;
    };

    // http:// or https://, a host (letters, digits, '-', '.', or an IPv4 address), an optional :port
    // (1..65535), then the path and query as printable ASCII (anything else percent-encoded); a query
    // needs a path ("https://host/?a=b"). No user info, no fragments, no spaces or control characters.
    bool ParseUrl(const char* url, UrlParts& parts);
    bool ValidHeaderName(const char* name);
    bool ValidHeaderValue(const char* value);
    bool ValidMethod(const char* method);
    // Everything above, plus: a body given one way at most, a file body with a descriptor and a length,
    // at most INT_MAX bytes (esp_http_client counts in int). Headers the HAL sets itself (Host,
    // Content-Length, Transfer-Encoding, Connection) are refused, in any case.
    bool ValidRequest(const Request& request);

    // Up to this much of a body file is read at once (into internal RAM: the SD card's DMA).
    inline constexpr std::size_t kUploadBlockBytes = 4096;

}  // namespace CardenzaHAL::Net
