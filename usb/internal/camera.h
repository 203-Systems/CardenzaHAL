#pragma once

#include <cstdint>

#include "tusb.h"

// The camera (usb/camera.cpp): the screen as a lossless stream on the WebUSB interface's bulk IN endpoint, which
// usb/usb.cpp opens when the camera is presented (Config::camera). Internal to the HAL.
namespace CardenzaHAL::USB::detail {

    // The interface's vendor requests from this range are the stream's (start, stop, a keyframe, ...).
    inline constexpr uint8_t kCameraRequestFirst = 0x20;
    inline constexpr uint8_t kCameraRequestLast = 0x2F;

    // usb.cpp → camera.cpp, on the USB task.
    void CameraOpened(bool open);  // the endpoint is configured (true) or gone (host reset, unplugged, reconfigured)
    void CameraSent(bool ok, uint32_t bytes);  // the transfer CameraTransfer started is done
    bool CameraControl(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request);

    // camera.cpp → usb.cpp: starts a bulk IN transfer of `data` (which stays valid until CameraSent). False when
    // the endpoint is not there or busy.
    bool CameraTransfer(const uint8_t* data, uint16_t length);

}  // namespace CardenzaHAL::USB::detail
