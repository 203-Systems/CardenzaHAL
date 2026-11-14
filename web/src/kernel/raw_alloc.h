#pragma once

#include <cstddef>

namespace cardenza_web {

// Memory for the web target itself (task stacks, buffers of the emulation): never counted in the
// firmware's emulated internal RAM or PSRAM (web/src/esp/heap.cpp), which malloc and heap_caps_* use.
void* RawAlloc(std::size_t bytes, std::size_t alignment = 16);
void RawFree(void* block);

}  // namespace cardenza_web
