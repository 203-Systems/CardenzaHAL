// CardenzaHAL::USB::Camera on the web target: the page shows the screen itself, there is no computer to stream to.
#include "usb/camera.h"

namespace CardenzaHAL::USB::Camera {

bool Enable(bool on) { return !on; }
bool Enabled() { return false; }
bool Streaming() { return false; }
Stats GetStats() { return Stats{}; }

}  // namespace CardenzaHAL::USB::Camera
