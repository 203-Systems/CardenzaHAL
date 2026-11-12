// The checks Net::Send makes before anything goes on the air. Pure C++ (no ESP-IDF): the host tests
// compile it too.
#include "net/http_request.h"

#include <climits>
#include <cstring>
#include <initializer_list>

namespace CardenzaHAL::Net {

namespace {

constexpr std::size_t kMaxHostLength = 253;

char Lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// `text` starts with `prefix`, ignoring case (for the scheme).
bool StartsWithNoCase(const char* text, const char* prefix)
{
    for (; *prefix != 0; ++text, ++prefix)
        if (Lower(*text) != *prefix) return false;
    return true;
}

bool EqualsNoCase(const char* a, const char* b)
{
    for (; *a != 0 && *b != 0; ++a, ++b)
        if (Lower(*a) != Lower(*b)) return false;
    return *a == *b;
}

bool HostCharacter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

// Printable ASCII without the space: what a path and query may hold as they are (anything else is
// percent-encoded by whoever made the URL).
bool UrlCharacter(char c)
{
    return c > 0x20 && c < 0x7F && c != '#';
}

bool TokenCharacter(char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    return std::strchr("!#$%&'*+-.^_`|~", c) != nullptr && c != 0;
}

}  // namespace

bool ParseUrl(const char* url, UrlParts& parts)
{
    parts = UrlParts{};
    if (url == nullptr) return false;
    const char* cursor = nullptr;
    if (StartsWithNoCase(url, "https://"))
    {
        parts.https = true;
        cursor = url + 8;
    }
    else if (StartsWithNoCase(url, "http://"))
    {
        cursor = url + 7;
    }
    else
    {
        return false;
    }

    // The host: letters, digits, '-' and '.', no empty label. User info ('@'), IPv6 literals ('[') and
    // anything else stop it and are refused below.
    const char* host = cursor;
    while (HostCharacter(*cursor)) ++cursor;
    const std::size_t hostLength = static_cast<std::size_t>(cursor - host);
    if (hostLength == 0 || hostLength > kMaxHostLength || host[0] == '.' || host[hostLength - 1] == '.') return false;
    for (std::size_t i = 1; i < hostLength; ++i)
        if (host[i] == '.' && host[i - 1] == '.') return false;

    uint32_t port = parts.https ? 443 : 80;
    if (*cursor == ':')
    {
        ++cursor;
        port = 0;
        std::size_t digits = 0;
        while (*cursor >= '0' && *cursor <= '9')
        {
            port = port * 10 + static_cast<uint32_t>(*cursor - '0');
            ++cursor;
            if (++digits > 5) return false;
        }
        if (digits == 0 || port == 0 || port > 65535) return false;
    }

    // The path, with the query: from the first '/' to the end. A query needs a path ("/?a=b").
    const char* path = cursor;
    if (*cursor != 0 && *cursor != '/') return false;
    for (; *cursor != 0; ++cursor)
        if (!UrlCharacter(*cursor)) return false;

    parts.host = host;
    parts.hostLength = hostLength;
    parts.port = static_cast<uint16_t>(port);
    if (*path == 0)
    {
        parts.path = "/";
        parts.pathLength = 1;
    }
    else
    {
        parts.path = path;
        parts.pathLength = static_cast<std::size_t>(cursor - path);
    }
    return true;
}

bool ValidHeaderName(const char* name)
{
    if (name == nullptr || *name == 0) return false;
    for (; *name != 0; ++name)
        if (!TokenCharacter(*name)) return false;
    return true;
}

bool ValidHeaderValue(const char* value)
{
    if (value == nullptr) return false;
    // Visible characters, spaces and tabs, and bytes from 0x80 (RFC 9110 field-value); no other control
    // character, so never CR or LF.
    for (; *value != 0; ++value)
    {
        const auto c = static_cast<unsigned char>(*value);
        if ((c < 0x20 && c != '\t') || c == 0x7F) return false;
    }
    return true;
}

bool ValidMethod(const char* method)
{
    if (method == nullptr) return false;
    for (const char* known : {"GET", "POST", "PUT", "DELETE", "HEAD", "PATCH"})
        if (std::strcmp(method, known) == 0) return true;
    return false;
}

bool ValidRequest(const Request& request)
{
    UrlParts parts;
    if (!ValidMethod(request.method) || !ParseUrl(request.url, parts)) return false;
    if (request.headerCount > 0 && request.headers == nullptr) return false;
    for (std::size_t i = 0; i < request.headerCount; ++i)
    {
        const Header& header = request.headers[i];
        if (!ValidHeaderName(header.name) || !ValidHeaderValue(header.value)) return false;
        for (const char* own : {"Host", "Content-Length", "Transfer-Encoding", "Connection"})
            if (EqualsNoCase(header.name, own)) return false;
    }
    const bool memoryBody = request.body != nullptr;
    const bool fileBody = request.bodyFd >= 0;
    if (!memoryBody && request.bodyLength != 0) return false;
    if (fileBody != (request.bodyFileLength != 0)) return false;
    if (memoryBody && fileBody) return false;
    // esp_http_client counts the body in an int.
    const uint64_t length = memoryBody ? request.bodyLength : request.bodyFileLength;
    return length <= static_cast<uint64_t>(INT_MAX);
}

}  // namespace CardenzaHAL::Net
