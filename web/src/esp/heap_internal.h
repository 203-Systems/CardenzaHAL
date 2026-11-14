#pragma once

#include <cstddef>
#include <cstdint>

namespace cardenza_web::heap {

// Memory the device takes from internal RAM for things the web target keeps elsewhere (a task's
// stack and TCB, a driver's buffers): a block of that size is held in the emulated internal RAM so
// what the firmware sees free matches the device. Null when it does not fit.
void* Charge(std::size_t bytes);
void Refund(void* charge);
std::size_t FreeInternalAndDefault();
std::size_t MinimumFreeInternalAndDefault();

// The PSRAM block that physical PSRAM addresses map to (esp_mmu_paddr_to_vaddr); null without PSRAM.
uint8_t* PsramBase();
std::size_t PsramBytes();
bool PsramEnabled();

// Summary for the boot log.
void PrintSummary();

}  // namespace cardenza_web::heap
