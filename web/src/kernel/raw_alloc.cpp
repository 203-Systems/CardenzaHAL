#include "raw_alloc.h"

#include <cstdint>
#include <cstdlib>

// The web build wraps malloc and friends (-Wl,--wrap=malloc, ...) so that the firmware's allocations
// land in the emulated RAMs; the web target's own memory comes from the real allocator underneath.
#if defined(CARDENZA_WEB_WRAP_MALLOC)
extern "C" void* __real_malloc(std::size_t);
extern "C" void __real_free(void*);
#define CARDENZA_RAW_MALLOC __real_malloc
#define CARDENZA_RAW_FREE __real_free
#else
#define CARDENZA_RAW_MALLOC std::malloc
#define CARDENZA_RAW_FREE std::free
#endif

namespace cardenza_web {

void* RawAlloc(std::size_t bytes, std::size_t alignment)
{
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    auto* raw = static_cast<uint8_t*>(CARDENZA_RAW_MALLOC(bytes + alignment + sizeof(void*)));
    if (raw == nullptr) return nullptr;
    uintptr_t aligned = reinterpret_cast<uintptr_t>(raw + sizeof(void*));
    aligned = (aligned + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
    reinterpret_cast<void**>(aligned)[-1] = raw;
    return reinterpret_cast<void*>(aligned);
}

void RawFree(void* block)
{
    if (block == nullptr) return;
    CARDENZA_RAW_FREE(static_cast<void**>(block)[-1]);
}

}  // namespace cardenza_web
