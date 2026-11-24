#pragma once

#include <cstdint>

// The USB camera: the device's screen as a lossless stream, made for recording and streaming tutorials (a web
// page shows and records it with the device's USB audio; a desktop program can make it a virtual camera for OBS).
//
// Exact pixels (RGB565, as the firmware presents them), no lossy compression: each frame carries only the
// rectangles that changed since the last one the computer read, raw or in a cheap run code (docs/screen-stream.md,
// the protocol; tools/web/screen-stream.js, the reference reader). It goes on a bulk IN endpoint of the "Cardenza
// WebUSB" interface (Kconfig CONFIG_CARDENZA_HAL_USB_CAMERA, which needs CONFIG_CARDENZA_HAL_USB_WEBUSB).
//
// What it costs:
//   - Off, or on with no computer reading it: nothing. The display's present paths test one pointer.
//   - While a computer reads it: at each framebuffer Present* the rows the firmware changed are compared with a
//     copy of the screen (PSRAM, 64.8 KB) and what differs is copied there (in the firmware's task: the time is
//     in Stats::captureUs); a task of priority 1 (3.5 KB stack) writes the frames into a 64.9 KB buffer (PSRAM)
//     and hands them to USB. A computer slower than the firmware gets fewer frames, each with everything that
//     changed since the last (never a backlog).
//   - One IN endpoint: Enable takes it from what gives way (usb/usb.h).
// What it shows: the frames the firmware presents with Display::Present(framebuffer...) or PresentChanged. A
// firmware that draws through the panel's handles (Display::Present() without a framebuffer) is not seen, nor is
// the system menu (the firmware's frames go on underneath it).
namespace CardenzaHAL::USB::Camera {

    // Presents the camera (true) or takes it away: the computer finds the device again with it (usb/usb.h,
    // Reconfigure). False when the build has no camera, USB is not started, or no endpoint can be had.
    bool Enable(bool on);
    // Presented.
    bool Enabled();
    // A computer has the stream open (started it and reads it).
    bool Streaming();

    struct Stats {
        bool enabled = false;    // the camera is presented
        bool open = false;       // a computer configured its endpoint
        bool streaming = false;  // a computer started the stream
        bool stalled = false;    // ... and has not read the last frame for 2 s: capturing paused
        uint32_t frames = 0;     // frames sent
        uint32_t keyframes = 0;
        uint32_t rects = 0;      // rectangles sent
        uint32_t presents = 0;   // the firmware's frames seen while streaming
        uint32_t coalesced = 0;  // of them, merged into a later frame (the computer was busy)
        uint32_t bytes = 0;      // sent
        uint32_t lastFrameBytes = 0;
        uint32_t sequence = 0;   // the next frame's
        uint32_t captureUs = 0, maxCaptureUs = 0;  // in the firmware's Present: the last one, the most
        uint32_t encodeUs = 0, maxEncodeUs = 0;    // the frame's writing (the stream's task): the last one, the most
        uint32_t errors = 0;     // no memory at start, a transfer that failed
    };
    Stats GetStats();

}  // namespace CardenzaHAL::USB::Camera
