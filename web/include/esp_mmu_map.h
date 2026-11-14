#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "hal/mmu_types.h"

CARDENZA_WEB_EXTERN_C_BEGIN
#define ESP_MMU_MMAP_FLAG_PADDR_SHARED BIT(0)
typedef uint32_t esp_paddr_t;
// The PSRAM's physical addresses map to the web target's PSRAM block. Flash is not mapped.
esp_err_t esp_mmu_paddr_to_vaddr(esp_paddr_t paddr, mmu_target_t target, mmu_vaddr_t type, void** out_vaddr);
esp_err_t esp_mmu_vaddr_to_paddr(void* vaddr, esp_paddr_t* out_paddr, mmu_target_t* out_target);
esp_err_t esp_mmu_map_get_max_consecutive_free_block_size(mmu_mem_caps_t caps, mmu_target_t target, size_t* out_len);
esp_err_t esp_mmu_map(esp_paddr_t paddr_start, size_t size, mmu_target_t target, mmu_mem_caps_t caps, int flags, void** out_ptr) CARDENZA_WEB_UNSUPPORTED;
esp_err_t esp_mmu_unmap(void* ptr) CARDENZA_WEB_UNSUPPORTED;
CARDENZA_WEB_EXTERN_C_END
