// Screenshots (screenshot/screenshot.h) on the web target: the page has no side key, no SD card and no presence
// service, and takes its own screenshots of the screen (the SCREENSHOT and FRAMED messages, AGENT.md). So the
// HAL's are absent: Take answers false (not taken), as it does where screenshots are not started. The API is here
// so that a firmware calling it builds for the web as it is.
#include "screenshot/screenshot.h"

namespace CardenzaHAL::Screenshot {

const char* StatusName(Status)
{
    return "absent on the web";
}

void Start() {}

bool Take(const Options&)
{
    return false;
}

void SetListener(Listener, void*) {}

bool IsShowing()
{
    return false;
}

void SetSpaceWait(bool) {}

bool SpaceWait()
{
    return false;
}

}  // namespace CardenzaHAL::Screenshot
