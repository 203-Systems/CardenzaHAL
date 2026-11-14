// The firmware's memory on the web target: internal RAM and PSRAM as regions with the device's room
// (region_allocator.h), and ESP-IDF's heap_caps_* on them. malloc, new and the C library's own
// allocations are routed here by -Wl,--wrap=malloc (and friends), as ESP-IDF routes them to its heaps:
// a firmware that runs out of internal RAM on the device runs out here too, at about the same point.
//
// The PSRAM follows the firmware's sdkconfig like ESP-IDF does:
//   CONFIG_SPIRAM_USE_MALLOC     malloc uses it: small blocks internal first, large ones PSRAM first
//   CONFIG_SPIRAM_USE_CAPS_ALLOC only heap_caps_malloc(..., MALLOC_CAP_SPIRAM) reaches it
//   CONFIG_SPIRAM_USE_MEMMAP     mapped but not in the heap: the firmware adds it
//                                (esp_mmu_paddr_to_vaddr + heap_caps_add_region_with_caps)
// Sizes: CONFIG_CARDENZA_WEB_INTERNAL_HEAP_SIZE (default 300 KB, about what a HAL firmware has free
// when app_main starts) and CONFIG_CARDENZA_WEB_PSRAM_SIZE (default the board's 2 MB).

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_heap_caps_init.h"
#include "esp_memory_utils.h"
#include "esp_mmu_map.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "sdkconfig.h"

#include "heap_internal.h"
#include "region_allocator.h"
#include "../kernel/raw_alloc.h"

#ifndef CONFIG_CARDENZA_WEB_INTERNAL_HEAP_SIZE
#define CONFIG_CARDENZA_WEB_INTERNAL_HEAP_SIZE (300 * 1024)
#endif
#ifndef CONFIG_CARDENZA_WEB_PSRAM_SIZE
#define CONFIG_CARDENZA_WEB_PSRAM_SIZE (2 * 1024 * 1024)
#endif
#ifndef CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
#define CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL 16384
#endif

namespace cardenza_web::heap {

namespace {

constexpr uint32_t kInternalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_32BIT | MALLOC_CAP_DMA |
                                   MALLOC_CAP_DEFAULT | MALLOC_CAP_DMA_DESC_AHB | MALLOC_CAP_CACHE_ALIGNED;
constexpr int kMaxRegions = 6;

// Plain data: malloc runs before static constructors do (other files' constructors allocate), so
// nothing here may have a constructor that would reset it afterwards.
struct Region {
    alignas(RegionAllocator) uint8_t storage[sizeof(RegionAllocator)];
    uint32_t caps;
    bool psram;
    bool used;
    RegionAllocator& Allocator() { return *reinterpret_cast<RegionAllocator*>(storage); }
};

Region g_regions[kMaxRegions];
int g_regionCount = 0;
bool g_initialized = false;
uint8_t* g_psram = nullptr;
std::size_t g_alwaysInternal = CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL;
esp_alloc_failed_hook_t g_failedHook = nullptr;

Region* AddRegion(void* start, std::size_t bytes, uint32_t caps, bool psram)
{
    if (g_regionCount >= kMaxRegions) return nullptr;
    Region& region = g_regions[g_regionCount++];
    new (region.storage) RegionAllocator(start, bytes);
    region.caps = caps;
    region.psram = psram;
    region.used = true;
    return &region;
}

bool PsramConfigured()
{
#if CONFIG_SPIRAM
    return true;
#else
    return false;
#endif
}

void EnsurePsramBlock()
{
    if (g_psram != nullptr || !PsramConfigured()) return;
    g_psram = static_cast<uint8_t*>(RawAlloc(CONFIG_CARDENZA_WEB_PSRAM_SIZE, 64));
}

void Init()
{
    if (g_initialized) return;
    g_initialized = true;
    void* internal = RawAlloc(CONFIG_CARDENZA_WEB_INTERNAL_HEAP_SIZE, 16);
    if (internal != nullptr) AddRegion(internal, CONFIG_CARDENZA_WEB_INTERNAL_HEAP_SIZE, kInternalCaps, false);
#if CONFIG_SPIRAM && (CONFIG_SPIRAM_USE_MALLOC || CONFIG_SPIRAM_USE_CAPS_ALLOC)
    EnsurePsramBlock();
    if (g_psram != nullptr)
    {
        uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_32BIT;
#if CONFIG_SPIRAM_USE_MALLOC
        caps |= MALLOC_CAP_DEFAULT;
#endif
        AddRegion(g_psram, CONFIG_CARDENZA_WEB_PSRAM_SIZE, caps, true);
    }
#endif
}

bool Matches(const Region& region, uint32_t caps)
{
    caps &= ~MALLOC_CAP_INVALID;
    return region.used && (region.caps & caps) == caps;
}

Region* RegionOf(const void* pointer)
{
    for (int index = 0; index < g_regionCount; ++index)
        if (g_regions[index].used && g_regions[index].Allocator().Owns(pointer)) return &g_regions[index];
    return nullptr;
}

void* AllocateIn(std::size_t size, uint32_t caps, std::size_t alignment, bool psramFirst)
{
    Init();
    for (int pass = 0; pass < 2; ++pass)
    {
        const bool wantPsram = (pass == 0) == psramFirst;
        for (int index = 0; index < g_regionCount; ++index)
        {
            Region& region = g_regions[index];
            if (region.psram != wantPsram || !Matches(region, caps)) continue;
            if (void* block = region.Allocator().Allocate(size, alignment)) return block;
        }
    }
    return nullptr;
}

void Failed(std::size_t size, uint32_t caps, const char* function)
{
    if (g_failedHook != nullptr) g_failedHook(size, caps, function);
}

void* MallocCaps(std::size_t size, uint32_t caps, std::size_t alignment, const char* function)
{
    if ((caps & (MALLOC_CAP_EXEC | MALLOC_CAP_IRAM_8BIT | MALLOC_CAP_RTCRAM | MALLOC_CAP_TCM)) != 0)
    {
        Failed(size, caps, function);
        return nullptr;  // no IRAM, RTC or TCM heap on the web target
    }
    void* block = AllocateIn(size, caps, alignment, false);
    if (block == nullptr) Failed(size, caps, function);
    return block;
}

// malloc(): ESP-IDF's heap_caps_malloc_default.
void* MallocDefault(std::size_t size, std::size_t alignment)
{
    Init();
    const bool psramFirst = size > g_alwaysInternal;
    void* block = AllocateIn(size, MALLOC_CAP_DEFAULT, alignment, psramFirst);
    if (block == nullptr) Failed(size, MALLOC_CAP_DEFAULT, "malloc");
    return block;
}

std::size_t Sum(uint32_t caps, std::size_t (RegionAllocator::*field)() const)
{
    Init();
    std::size_t total = 0;
    for (int index = 0; index < g_regionCount; ++index)
        if (Matches(g_regions[index], caps)) total += (g_regions[index].Allocator().*field)();
    return total;
}

}  // namespace

void* Charge(std::size_t bytes)
{
    return AllocateIn(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, 8, false);
}

void Refund(void* charge)
{
    if (Region* region = RegionOf(charge)) region->Allocator().Free(charge);
}

std::size_t FreeInternalAndDefault() { return Sum(MALLOC_CAP_DEFAULT, &RegionAllocator::FreeBytes); }
std::size_t MinimumFreeInternalAndDefault() { return Sum(MALLOC_CAP_DEFAULT, &RegionAllocator::MinimumFreeBytes); }

uint8_t* PsramBase()
{
    EnsurePsramBlock();
    return g_psram;
}

std::size_t PsramBytes() { return PsramConfigured() ? CONFIG_CARDENZA_WEB_PSRAM_SIZE : 0; }
bool PsramEnabled() { return PsramConfigured(); }

void PrintSummary()
{
    Init();
    std::printf("I (%u) heap_init: internal RAM %u KB free", static_cast<unsigned>(esp_log_timestamp()),
                static_cast<unsigned>(Sum(MALLOC_CAP_INTERNAL, &RegionAllocator::FreeBytes) / 1024));
    if (PsramConfigured())
    {
#if CONFIG_SPIRAM_USE_MALLOC
        const char* mode = "in malloc";
#elif CONFIG_SPIRAM_USE_CAPS_ALLOC
        const char* mode = "MALLOC_CAP_SPIRAM only";
#else
        const char* mode = "mapped, not in the heap (CONFIG_SPIRAM_USE_MEMMAP)";
#endif
        std::printf(", PSRAM %u KB (%s)", static_cast<unsigned>(PsramBytes() / 1024), mode);
    }
    std::printf("\n");
}

}  // namespace cardenza_web::heap

namespace h = cardenza_web::heap;

// ---- malloc and friends (-Wl,--wrap=...) ------------------------------------------------------

extern "C" {

void* __real_malloc(size_t);
void __real_free(void*);
void* __real_realloc(void*, size_t);
size_t __real_malloc_usable_size(void*);

void* __wrap_malloc(size_t size) { return h::MallocDefault(size, 8); }

void __wrap_free(void* block)
{
    if (block == nullptr) return;
    if (h::Region* region = h::RegionOf(block)) region->Allocator().Free(block);
    else __real_free(block);  // from the JavaScript side or the web target itself
}

void* __wrap_calloc(size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size) return nullptr;
    void* block = h::MallocDefault(count * size, 8);
    if (block != nullptr) std::memset(block, 0, count * size);
    return block;
}

void* __wrap_realloc(void* block, size_t size)
{
    if (block == nullptr) return h::MallocDefault(size, 8);
    h::Region* region = h::RegionOf(block);
    if (region == nullptr) return __real_realloc(block, size);
    if (size == 0)
    {
        region->Allocator().Free(block);
        return nullptr;
    }
    if (void* moved = region->Allocator().Reallocate(block, size)) return moved;
    // No room in its own RAM: anywhere malloc may go.
    void* moved = h::MallocDefault(size, 8);
    if (moved == nullptr) return nullptr;
    const std::size_t have = region->Allocator().UsableSize(block);
    std::memcpy(moved, block, have < size ? have : size);
    region->Allocator().Free(block);
    return moved;
}

void* __wrap_memalign(size_t alignment, size_t size) { return h::MallocDefault(size, alignment); }
void* __wrap_aligned_alloc(size_t alignment, size_t size) { return h::MallocDefault(size, alignment); }

int __wrap_posix_memalign(void** out, size_t alignment, size_t size)
{
    if (alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0) return EINVAL;
    void* block = h::MallocDefault(size, alignment);
    if (block == nullptr) return ENOMEM;
    *out = block;
    return 0;
}

size_t __wrap_malloc_usable_size(void* block)
{
    if (h::Region* region = h::RegionOf(block)) return region->Allocator().UsableSize(block);
    return block != nullptr ? __real_malloc_usable_size(block) : 0;
}

// ---- heap_caps ----------------------------------------------------------------------------------

esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback)
{
    if (callback == nullptr) return ESP_ERR_INVALID_ARG;
    h::g_failedHook = callback;
    return ESP_OK;
}

void* heap_caps_malloc(size_t size, uint32_t caps) { return h::MallocCaps(size, caps, 8, __func__); }
void heap_caps_free(void* ptr) { __wrap_free(ptr); }

void* heap_caps_realloc(void* ptr, size_t size, uint32_t caps)
{
    if (ptr == nullptr) return heap_caps_malloc(size, caps);
    if (size == 0)
    {
        heap_caps_free(ptr);
        return nullptr;
    }
    h::Region* region = h::RegionOf(ptr);
    if (region != nullptr && h::Matches(*region, caps))
        if (void* moved = region->Allocator().Reallocate(ptr, size)) return moved;
    void* moved = heap_caps_malloc(size, caps);
    if (moved == nullptr) return nullptr;
    const std::size_t have = region != nullptr ? region->Allocator().UsableSize(ptr) : size;
    std::memcpy(moved, ptr, have < size ? have : size);
    heap_caps_free(ptr);
    return moved;
}

void* heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
{
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;
    return h::MallocCaps(size, caps, alignment, __func__);
}

void heap_caps_aligned_free(void* ptr) { heap_caps_free(ptr); }

void* heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, uint32_t caps)
{
    if (size != 0 && n > SIZE_MAX / size) return nullptr;
    void* block = heap_caps_aligned_alloc(alignment, n * size, caps);
    if (block != nullptr) std::memset(block, 0, n * size);
    return block;
}

void* heap_caps_calloc(size_t n, size_t size, uint32_t caps)
{
    if (size != 0 && n > SIZE_MAX / size) return nullptr;
    void* block = heap_caps_malloc(n * size, caps);
    if (block != nullptr) std::memset(block, 0, n * size);
    return block;
}

size_t heap_caps_get_total_size(uint32_t caps) { return h::Sum(caps, &h::RegionAllocator::TotalBytes); }
size_t heap_caps_get_free_size(uint32_t caps) { return h::Sum(caps, &h::RegionAllocator::FreeBytes); }
size_t heap_caps_get_minimum_free_size(uint32_t caps) { return h::Sum(caps, &h::RegionAllocator::MinimumFreeBytes); }

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    h::Init();
    std::size_t largest = 0;
    for (int index = 0; index < h::g_regionCount; ++index)
        if (h::Matches(h::g_regions[index], caps))
        {
            const std::size_t block = h::g_regions[index].Allocator().LargestFreeBlock();
            if (block > largest) largest = block;
        }
    return largest;
}

esp_err_t heap_caps_monitor_local_minimum_free_size_start(void) { return ESP_OK; }
esp_err_t heap_caps_monitor_local_minimum_free_size_stop(void) { return ESP_OK; }

void heap_caps_get_info(multi_heap_info_t* info, uint32_t caps)
{
    std::memset(info, 0, sizeof(*info));
    h::Init();
    for (int index = 0; index < h::g_regionCount; ++index)
    {
        if (!h::Matches(h::g_regions[index], caps)) continue;
        const h::RegionAllocator& allocator = h::g_regions[index].Allocator();
        info->total_free_bytes += allocator.FreeBytes();
        info->total_allocated_bytes += allocator.AllocatedBytes();
        info->minimum_free_bytes += allocator.MinimumFreeBytes();
        info->allocated_blocks += allocator.AllocatedBlocks();
        info->free_blocks += allocator.FreeBlocks();
        info->total_blocks += allocator.AllocatedBlocks() + allocator.FreeBlocks();
        if (allocator.LargestFreeBlock() > info->largest_free_block) info->largest_free_block = allocator.LargestFreeBlock();
    }
}

void heap_caps_print_heap_info(uint32_t caps)
{
    h::Init();
    std::printf("Heap summary for capabilities 0x%08X:\n", static_cast<unsigned>(caps));
    for (int index = 0; index < h::g_regionCount; ++index)
    {
        if (!h::Matches(h::g_regions[index], caps)) continue;
        const h::RegionAllocator& allocator = h::g_regions[index].Allocator();
        std::printf("  At 0x%08lx len %u free %u allocated %u min_free %u\n    largest_free_block %u alloc_blocks %u free_blocks %u (%s)\n",
                    static_cast<unsigned long>(allocator.Start()), static_cast<unsigned>(allocator.TotalBytes()),
                    static_cast<unsigned>(allocator.FreeBytes()), static_cast<unsigned>(allocator.AllocatedBytes()),
                    static_cast<unsigned>(allocator.MinimumFreeBytes()), static_cast<unsigned>(allocator.LargestFreeBlock()),
                    static_cast<unsigned>(allocator.AllocatedBlocks()), static_cast<unsigned>(allocator.FreeBlocks()),
                    h::g_regions[index].psram ? "PSRAM" : "internal");
    }
    std::printf("  Totals:\n    free %u allocated %u min_free %u largest_free_block %u\n",
                static_cast<unsigned>(heap_caps_get_free_size(caps)),
                static_cast<unsigned>(heap_caps_get_total_size(caps) - heap_caps_get_free_size(caps)),
                static_cast<unsigned>(heap_caps_get_minimum_free_size(caps)), static_cast<unsigned>(heap_caps_get_largest_free_block(caps)));
}

bool heap_caps_check_integrity_all(bool print_errors) { return heap_caps_check_integrity(0, print_errors); }

bool heap_caps_check_integrity(uint32_t caps, bool print_errors)
{
    h::Init();
    bool ok = true;
    for (int index = 0; index < h::g_regionCount; ++index)
    {
        if (!h::Matches(h::g_regions[index], caps)) continue;
        if (!h::g_regions[index].Allocator().Check())
        {
            ok = false;
            if (print_errors) std::printf("CORRUPT HEAP: region at 0x%08lx\n", static_cast<unsigned long>(h::g_regions[index].Allocator().Start()));
        }
    }
    return ok;
}

bool heap_caps_check_integrity_addr(intptr_t addr, bool print_errors)
{
    h::Region* region = h::RegionOf(reinterpret_cast<void*>(addr));
    if (region == nullptr) return false;
    const bool ok = region->Allocator().Check();
    if (!ok && print_errors) std::printf("CORRUPT HEAP around 0x%08lx\n", static_cast<unsigned long>(addr));
    return ok;
}

void heap_caps_malloc_extmem_enable(size_t limit) { h::g_alwaysInternal = limit; }

void* heap_caps_malloc_prefer(size_t size, size_t num, ...)
{
    va_list args;
    va_start(args, num);
    void* block = nullptr;
    while (num-- > 0 && block == nullptr) block = heap_caps_malloc(size, va_arg(args, uint32_t));
    va_end(args);
    return block;
}

void* heap_caps_realloc_prefer(void* ptr, size_t size, size_t num, ...)
{
    va_list args;
    va_start(args, num);
    void* block = nullptr;
    while (num-- > 0 && block == nullptr) block = heap_caps_realloc(ptr, size, va_arg(args, uint32_t));
    va_end(args);
    return block;
}

void* heap_caps_calloc_prefer(size_t n, size_t size, size_t num, ...)
{
    va_list args;
    va_start(args, num);
    void* block = nullptr;
    while (num-- > 0 && block == nullptr) block = heap_caps_calloc(n, size, va_arg(args, uint32_t));
    va_end(args);
    return block;
}

void heap_caps_dump(uint32_t caps) { heap_caps_print_heap_info(caps); }
void heap_caps_dump_all(void) { heap_caps_print_heap_info(0); }

size_t heap_caps_get_allocated_size(void* ptr)
{
    h::Region* region = h::RegionOf(ptr);
    return region != nullptr ? region->Allocator().UsableSize(ptr) : 0;
}

void heap_caps_init(void) { h::Init(); }
void heap_caps_enable_nonos_stack_heaps(void) {}

esp_err_t heap_caps_add_region(intptr_t start, intptr_t end)
{
    const uint32_t caps[3] = {MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT, 0, MALLOC_CAP_32BIT};
    return heap_caps_add_region_with_caps(caps, start, end);
}

esp_err_t heap_caps_add_region_with_caps(const uint32_t caps[], intptr_t start, intptr_t end)
{
    h::Init();
    if (caps == nullptr || end <= start) return ESP_ERR_INVALID_ARG;
    for (int index = 0; index < h::g_regionCount; ++index)
    {
        const h::RegionAllocator& allocator = h::g_regions[index].Allocator();
        if (static_cast<uintptr_t>(start) < allocator.End() && static_cast<uintptr_t>(end) > allocator.Start()) return ESP_ERR_INVALID_STATE;
    }
    const uint32_t all = caps[0] | caps[1] | caps[2];
    const bool psram = esp_ptr_external_ram(reinterpret_cast<void*>(start));
    if (h::AddRegion(reinterpret_cast<void*>(start), static_cast<std::size_t>(end - start), all, psram) == nullptr) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

// ---- where a pointer points ---------------------------------------------------------------------

bool esp_ptr_external_ram(const void* p)
{
    const auto address = reinterpret_cast<uintptr_t>(p);
    if (h::g_psram != nullptr && address >= reinterpret_cast<uintptr_t>(h::g_psram) &&
        address < reinterpret_cast<uintptr_t>(h::g_psram) + CONFIG_CARDENZA_WEB_PSRAM_SIZE)
        return true;
    const h::Region* region = h::RegionOf(p);
    return region != nullptr && region->psram;
}

bool esp_ptr_internal(const void* p) { return p != nullptr && !esp_ptr_external_ram(p); }
bool esp_ptr_dma_capable(const void* p) { return esp_ptr_internal(p); }
bool esp_ptr_dma_ext_capable(const void* p) { return esp_ptr_external_ram(p); }
bool esp_ptr_word_aligned(const void* p) { return (reinterpret_cast<uintptr_t>(p) & 3) == 0; }
bool esp_ptr_executable(const void*) { return false; }
bool esp_ptr_byte_accessible(const void* p) { return p != nullptr; }
bool esp_ptr_in_dram(const void* p) { return esp_ptr_internal(p); }
bool esp_ptr_in_iram(const void*) { return false; }
bool esp_ptr_in_drom(const void*) { return false; }
bool esp_ptr_in_rtc_slow(const void*) { return false; }
bool esp_ptr_in_rtc_dram_fast(const void*) { return false; }
bool esp_ptr_in_diram_dram(const void* p) { return esp_ptr_internal(p); }
bool esp_stack_ptr_is_sane(uint32_t sp) { return sp != 0 && (sp & 3) == 0; }
bool esp_ptr_in_visible_dram(const void* p) { return esp_ptr_internal(p); }

// ---- PSRAM and its mapping ----------------------------------------------------------------------

bool esp_psram_is_initialized(void) { return h::PsramConfigured(); }
size_t esp_psram_get_size(void) { return h::PsramBytes(); }
esp_err_t esp_psram_init(void) { return h::PsramConfigured() ? ESP_OK : ESP_FAIL; }

esp_err_t esp_mmu_paddr_to_vaddr(esp_paddr_t paddr, mmu_target_t target, mmu_vaddr_t type, void** out_vaddr)
{
    if (out_vaddr == nullptr) return ESP_ERR_INVALID_ARG;
    if (target != MMU_TARGET_PSRAM0 || type != MMU_VADDR_DATA) return ESP_ERR_NOT_FOUND;
    uint8_t* base = h::PsramBase();
    if (base == nullptr || paddr >= h::PsramBytes()) return ESP_ERR_NOT_FOUND;
    *out_vaddr = base + paddr;
    return ESP_OK;
}

esp_err_t esp_mmu_vaddr_to_paddr(void* vaddr, esp_paddr_t* out_paddr, mmu_target_t* out_target)
{
    const auto address = reinterpret_cast<uintptr_t>(vaddr);
    const auto base = reinterpret_cast<uintptr_t>(h::g_psram);
    if (h::g_psram == nullptr || address < base || address >= base + h::PsramBytes()) return ESP_ERR_NOT_FOUND;
    if (out_paddr != nullptr) *out_paddr = static_cast<esp_paddr_t>(address - base);
    if (out_target != nullptr) *out_target = MMU_TARGET_PSRAM0;
    return ESP_OK;
}

esp_err_t esp_mmu_map_get_max_consecutive_free_block_size(mmu_mem_caps_t, mmu_target_t, size_t* out_len)
{
    if (out_len == nullptr) return ESP_ERR_INVALID_ARG;
    *out_len = 0;
    return ESP_OK;
}

// ---- ESP-IDF's heap totals (esp_system.h) -------------------------------------------------------

uint32_t esp_get_free_heap_size(void) { return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_DEFAULT)); }
uint32_t esp_get_free_internal_heap_size(void)
{
    return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
}
uint32_t esp_get_minimum_free_heap_size(void) { return static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT)); }

}  // extern "C"
