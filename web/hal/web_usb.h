#pragma once

#include <cstddef>
#include <cstdint>

// The page's side of the web USB (usb.cpp): what the host sends.
namespace cardenza_web::usb {

void MidiFromHost(const uint8_t packet[4]);
void AppFromHost(const uint8_t* data, std::size_t length);

}  // namespace cardenza_web::usb
