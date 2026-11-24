// HID inputs for a firmware (usb/usb.h, KeyboardReport and ConsumerReport): their part of the HID interface's
// report descriptor. They share the interface, and its one IN endpoint, with the system channel (report 0xCB)
// and the firmware's raw reports (0xFF); usb/usb.cpp sends them in order.
#include <cstring>

#include "usb/internal/dev.h"
#include "tusb.h"

namespace CardenzaHAL::USB::detail {

#if CFG_TUD_HID
namespace {
enum : uint8_t { kReportKeyboard = 1, kReportConsumer = 3, kReportGamepad = 4 };  // as in Matrix OS
}  // namespace

std::size_t HidInputReportDescriptor(uint8_t* out, std::size_t capacity, bool keyboard, bool consumer, bool gamepad)
{
    std::size_t length = 0;
    const auto add = [&](const uint8_t* block, std::size_t size) {
        if (length + size > capacity) return;
        std::memcpy(out + length, block, size);
        length += size;
    };
    if (keyboard)
    {
        const uint8_t block[] = {TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(kReportKeyboard))};
        add(block, sizeof(block));
    }
    if (consumer)
    {
        const uint8_t block[] = {TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(kReportConsumer))};
        add(block, sizeof(block));
    }
    if (gamepad)
    {
        const uint8_t block[] = {TUD_HID_REPORT_DESC_GAMEPAD(HID_REPORT_ID(kReportGamepad))};
        add(block, sizeof(block));
    }
    return length;
}
#else
std::size_t HidInputReportDescriptor(uint8_t*, std::size_t, bool, bool, bool) { return 0; }
#endif

void HidInputReset() {}

}  // namespace CardenzaHAL::USB::detail
