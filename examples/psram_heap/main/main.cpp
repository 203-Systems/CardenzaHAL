// PSRAM in the heap only for what asks for it, and `new` that never reboots the board.
//
// Cardenza has two RAMs. Internal RAM (about 300 KB of heap) is fast and the only one the SD card's
// DMA, interrupts and code running while the flash is written can use. PSRAM (2 MB, on the flash
// bus) is slower and fine for data read now and then: pattern steps, snapshots, samples, lists.
//
// - With CONFIG_SPIRAM_USE_MALLOC (ESP-IDF's default) malloc spills into PSRAM by itself, and a
//   driver or a DMA buffer may land there. Here (CONFIG_SPIRAM_USE_MEMMAP) the PSRAM is mapped but
//   added to the heap with MALLOC_CAP_SPIRAM alone: malloc and MALLOC_CAP_DEFAULT never reach it, only
//   code that asks for it does (heap_caps_malloc(size, MALLOC_CAP_SPIRAM)).
// - A firmware built without exceptions (ESP-IDF's default) aborts when plain `new` fails, and
//   libstdc++'s `new (std::nothrow)` calls plain new, so it aborts too. The replacements below make
//   nothrow new return null, and let both spill into PSRAM when internal RAM is full.
// - Measure internal RAM with MALLOC_CAP_INTERNAL: heap_caps_get_free_size(MALLOC_CAP_8BIT) counts
//   the PSRAM too.
#include <cstdint>
#include <cstdlib>
#include <new>

#include <esp_heap_caps.h>
#include <esp_heap_caps_init.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <esp_mmu_map.h>
#include <esp_psram.h>
#include <sdkconfig.h>

namespace {

constexpr char kTag[] = "psram_heap";
constexpr uint32_t kInternalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t kSlowCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
// Large nothrow blocks go to PSRAM rather than leave less than this much internal RAM; the small
// allocations of everything else need it. Tune both to your firmware.
constexpr size_t kLargeBytes = 4 * 1024;
constexpr size_t kInternalFloorBytes = 20 * 1024;

// The mapped PSRAM joins the heap with its own capability. Call it first thing in app_main, before
// any task that allocates.
void AddPsramToHeap()
{
#if CONFIG_SPIRAM
    if (!esp_psram_is_initialized())
    {
        ESP_LOGW(kTag, "no PSRAM: everything stays in internal RAM");
        return;
    }
    const size_t size = esp_psram_get_size();
    void* first = nullptr;
    void* last = nullptr;
    if (size == 0 || esp_mmu_paddr_to_vaddr(0, MMU_TARGET_PSRAM0, MMU_VADDR_DATA, &first) != ESP_OK ||
        esp_mmu_paddr_to_vaddr(size - 1, MMU_TARGET_PSRAM0, MMU_VADDR_DATA, &last) != ESP_OK ||
        static_cast<char*>(last) != static_cast<char*>(first) + size - 1)
    {
        ESP_LOGE(kTag, "PSRAM of %u bytes is not mapped in one piece: not used", static_cast<unsigned>(size));
        return;
    }
    // Priority 0 says what the region is (SPIRAM); malloc's default caps never match it.
    const uint32_t caps[] = {MALLOC_CAP_SPIRAM, 0, MALLOC_CAP_8BIT | MALLOC_CAP_32BIT};
    const intptr_t start = reinterpret_cast<intptr_t>(first);
    if (heap_caps_add_region_with_caps(caps, start, start + static_cast<intptr_t>(size)) != ESP_OK)
    {
        ESP_LOGE(kTag, "PSRAM could not be added to the heap");
        return;
    }
    ESP_LOGI(kTag, "PSRAM: %u bytes at %p, for what asks for it", static_cast<unsigned>(size), first);
#endif
}

void* InternalElsePsram(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL, kSlowCaps);
}

void* AllocateOrNull(size_t size)
{
    if (size == 0) size = 1;
    if (size >= kLargeBytes && heap_caps_get_free_size(kInternalCaps) < size + kInternalFloorBytes)
        return heap_caps_malloc(size, kSlowCaps);
    return InternalElsePsram(size);
}

}  // namespace

// Plain new: internal RAM, PSRAM when that is full; the end of the program only when both are.
void* operator new(std::size_t size)
{
    if (void* block = InternalElsePsram(size != 0 ? size : 1)) return block;
    std::abort();
}

void* operator new[](std::size_t size)
{
    return operator new(size);
}

// Nothrow new: null when there is no room, never an abort. Use it for anything large, and say so
// on screen when it fails.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    return AllocateOrNull(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    return AllocateOrNull(size);
}

extern "C" void app_main(void)
{
    AddPsramToHeap();
    ESP_LOGI(kTag, "internal RAM free: %u bytes, PSRAM free: %u bytes",
             static_cast<unsigned>(heap_caps_get_free_size(kInternalCaps)),
             static_cast<unsigned>(heap_caps_get_free_size(kSlowCaps)));
    // Data read now and then goes to PSRAM on purpose.
    auto* steps = static_cast<uint8_t*>(heap_caps_malloc(64 * 1024, kSlowCaps));
    // A large optional block: null instead of a reboot when there is no room.
    auto* scratch = new (std::nothrow) uint8_t[512 * 1024];
    ESP_LOGI(kTag, "64 KB in PSRAM: %s; 512 KB nothrow: %s (%s)", steps != nullptr ? "yes" : "no",
             scratch != nullptr ? "yes" : "null, no room",
             scratch != nullptr && esp_ptr_external_ram(scratch) ? "PSRAM" : "internal");
    delete[] scratch;
    heap_caps_free(steps);
}
