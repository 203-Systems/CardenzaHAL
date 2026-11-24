// The camera (usb/usb.h, Config::camera): the screen as a lossless stream on the WebUSB interface, for recording
// and streaming tutorials.
#include "usb/internal/camera.h"
#include "usb/internal/dev.h"

namespace CardenzaHAL::USB::detail {

void CameraOpened(bool) {}
void CameraSent(bool, uint32_t) {}
bool CameraControl(uint8_t, uint8_t, tusb_control_request_t const*) { return false; }

}  // namespace CardenzaHAL::USB::detail
