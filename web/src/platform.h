#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_system.h"

// The page around the firmware (web_main.cpp; the JavaScript side is web/page/cardenza-sim.js).
namespace cardenza_web::platform {

// esp_restart and friends: the page starts a new instance of the firmware. Never returns.
[[noreturn]] void Restart(esp_reset_reason_t reason);
esp_reset_reason_t ResetReason();
uint32_t Random32();

// The SD card the page gives this run: none, or one of this many bytes.
bool CardPresent();
uint64_t CardBytes();
// Files changed: the page writes them to IndexedDB when it keeps them.
void FilesChanged();

// What the device would send that the page shows or passes on instead.
void IrSent(uint8_t address, uint8_t command, bool nec);
void MidiOut(const uint8_t packet[4]);
void AppReport(const uint8_t* data, std::size_t length);
void UsbStarted(const char* product);

// The firmware's app_main is running: the page tells its parent it booted.
void Booted();

}  // namespace cardenza_web::platform
