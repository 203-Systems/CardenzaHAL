#pragma once

#include <cstddef>
#include <cstdint>

#include "net/http_request.h"

// Work the presence service (net/presence.h) does for the rest of the HAL while it is online: the screenshots'
// uploads (screenshot/screenshot.h). Internal to the HAL.
namespace CardenzaHAL::Presence::detail {

    // Given to the side work for the length of its call, on the service's task (priority 1), after an inbox
    // request the community answered while nothing held the service back (the firmware's activity, the audio).
    struct SideRequests {
        // POST <api><path> with the device token, the service's headers (the firmware's name, version and image
        // digest) and `extra`, the body `bytes` bytes read from `fd`. The HTTP status (0: no answer); the answer's
        // body into `answer` (zero-terminated) when there is room.
        int (*post)(void* context, const char* path, const Net::Header* extra, std::size_t extraCount, int fd,
                    uint64_t bytes, char* answer, std::size_t capacity);
        // The service wants the network back now (switched off, the firmware's activity, the audio late): stop.
        bool (*stop)(void* context);
        void* context;
    };
    using SideWork = void (*)(const SideRequests& requests);
    // One at a time (the screenshots'); nullptr: none. It decides itself, quickly, whether it has anything to do.
    void SetSideWork(SideWork work);

}  // namespace CardenzaHAL::Presence::detail
