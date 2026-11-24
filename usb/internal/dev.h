#pragma once

#include <cstddef>
#include <cstdint>

#include "usb/usb_logic.h"

// Inside the USB device (usb/usb.cpp, which carries the classes) and the parts in their own files: the
// development channel (usb/dev.cpp, docs/usb-dev.md), HID inputs (usb/hid.cpp), the drive (usb/drive.cpp) and
// the camera (usb/camera.cpp). Internal to the HAL.
namespace CardenzaHAL::USB::detail {

    // How a request came, and so how its answer goes back.
    enum class Way : uint8_t {
        Hid,     // report 0xCB
        Sysex,   // F0 23 ... F7 / F0 24 ... F7
        Serial,  // the serial port's "sys <hex>" lines
        Text,    // the serial port's words ("info"): the answer's payload as it is, one line
        WebUsb,  // control transfers to the WebUSB interface
    };

    // ---- The development channel -------------------------------------------------------------------------

    // usb.cpp → dev.cpp, on the USB task. A request: what followed the command byte (id, op, args; the args
    // packed when it came as SysEx; for Text only id and op).
    void DevRequest(const uint8_t* data, std::size_t length, Way way);
    // The computer went away: keys it held are let go, an answer under way stops.
    void DevHostGone();

    // dev.cpp → usb.cpp. One answer message: a report 0xCB (63 bytes) over HID, the serial port or WebUSB, or a
    // whole SysEx (F0 … F7), or for Text a piece of the answer (flags: dev_logic.h kFlagLast, kFlagError). They
    // wait up to `waitMs` for room, and give up when the computer is gone. Never wait from the USB task
    // (waitMs 0): it is the one that makes room.
    bool DevSendHid(const uint8_t report[63], uint32_t waitMs);
    bool DevSendSysex(const uint8_t* message, std::size_t length, uint32_t waitMs);
    bool DevSendSerial(const uint8_t report[63], uint32_t waitMs);
    bool DevSendWebUsb(const uint8_t report[63], uint32_t waitMs);
    bool DevSendText(const uint8_t* bytes, std::size_t length, uint8_t flags, uint32_t waitMs);
    // The firmware's name (USB::Config::product).
    const char* Product();

    // What the device is, for INFO.
    struct Identity {
        uint16_t vendorId = 0, productId = 0;
        char serialNumber[18] = {};  // the MAC, "70:AF:09:BC:EB:30"
        Logic::Classes classes;      // what it presents
        bool started = false;
    };
    Identity CurrentIdentity();

    // ---- The classes in their own files ----------------------------------------------------------------------

    // Changes what the device presents (the computer finds it again), making room for `keep` by what gives way
    // (Logic::Fit). From the firmware's task, never the USB task. False when the device is not started or the
    // classes cannot be had (not built, or no room even so).
    bool SetClasses(const Logic::Classes& wanted, const Logic::Classes& keep);
    Logic::Classes AskedClasses();

    // HID inputs (usb/hid.cpp): their part of the report descriptor, and their reports, which leave one at a
    // time with the system channel's on the HID endpoint (the USB task sends them). QueueInput waits up to
    // `waitMs` for room; false without HID, without a host, or when the queue stayed full.
    std::size_t HidInputReportDescriptor(uint8_t* out, std::size_t capacity, bool keyboard, bool consumer, bool gamepad);
    bool QueueInput(uint8_t reportId, const uint8_t* data, std::size_t length, uint32_t waitMs);
    // The host is gone or the device changed: the inputs' state is cleared (nothing stays pressed).
    void HidInputReset();
    // USB task, every round: what the computer has not got yet goes when it can (a computer that comes back gets
    // the state as it is). Never waits.
    void HidInputPump();
    // The keyboard's output report (report ID 1): the lights the computer set (usb/hid.h, KeyboardLeds).
    void HidKeyboardLeds(uint8_t leds);

    // The drive (usb/drive.cpp): its descriptor (interface `itf`, string `stringIndex`), 0 when it has none to
    // give; told when the host goes or comes.
    std::size_t DriveDescriptor(uint8_t* out, std::size_t capacity, uint8_t itf, uint8_t stringIndex, uint8_t epOut, uint8_t epIn);
    void DriveHostGone();
    // The bus suspended (the host sleeps, or a board on its own power lost the cable) or resumed; from the USB task.
    void DriveSuspended(bool suspended);

    // The camera (the screen stream): usb/internal/camera.h.

    // The USB network (usb/network.cpp): its descriptor (an interface association of two interfaces from `itf`;
    // `macStringIndex` names the string NetworkMacString answers), 0 when it has none to give; the MAC the
    // computer's side of the link takes, as 12 hex digits.
    std::size_t NetworkDescriptor(uint8_t* out, std::size_t capacity, uint8_t itf, uint8_t stringIndex, uint8_t macStringIndex,
                                  uint8_t epNotify, uint8_t epOut, uint8_t epIn);
    const char* NetworkMacString();

}  // namespace CardenzaHAL::USB::detail
