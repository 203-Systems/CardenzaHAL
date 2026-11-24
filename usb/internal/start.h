#pragma once

// CardenzaHAL::Init → usb/usb.cpp (Kconfig CONFIG_CARDENZA_HAL_USB_AUTOSTART): starts the device with the default
// configuration, connected a second later unless the firmware's USB::Init takes it over first. Internal.
namespace CardenzaHAL::USB::detail {

    void AutoStart();

}  // namespace CardenzaHAL::USB::detail
