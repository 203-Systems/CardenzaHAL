// What the kernel's FreeRTOS layer needs from the rest of the web target, on the host: the heap
// charges are plain allocations (heap.cpp itself is the web build's: it wraps malloc).
#include <cstdlib>
#include "web/src/esp/heap_internal.h"

namespace cardenza_web::heap {
void* Charge(std::size_t bytes) { return std::malloc(bytes != 0 ? bytes : 1); }
void Refund(void* charge) { std::free(charge); }
std::size_t FreeInternalAndDefault() { return 300 * 1024; }
std::size_t MinimumFreeInternalAndDefault() { return 300 * 1024; }
uint8_t* PsramBase() { return nullptr; }
std::size_t PsramBytes() { return 0; }
bool PsramEnabled() { return false; }
void PrintSummary() {}
}  // namespace cardenza_web::heap
