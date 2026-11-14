#pragma once

// FAT on the SD card, mounted at a path (the HAL's Storage::Init: /sdcard). On the web target the
// files are the page's (Emscripten's file system; persisted in IndexedDB when the page may).
// Differences from FAT: names are case-sensitive and an open file can be renamed or deleted.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/sdmmc_types.h"
#include "driver/sdmmc_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool format_if_mount_failed;
    int max_files;
    size_t allocation_unit_size;
    bool disk_status_check_enable;
    bool use_one_fat;
} esp_vfs_fat_mount_config_t;
typedef esp_vfs_fat_mount_config_t esp_vfs_fat_sdmmc_mount_config_t;
typedef int32_t wl_handle_t;

esp_err_t esp_vfs_fat_sdmmc_mount(const char* base_path, const sdmmc_host_t* host_config, const void* slot_config,
                                  const esp_vfs_fat_mount_config_t* mount_config, sdmmc_card_t** out_card);
esp_err_t esp_vfs_fat_sdcard_unmount(const char* base_path, sdmmc_card_t* card);
esp_err_t esp_vfs_fat_sdmmc_unmount(void) __attribute__((deprecated("Please use esp_vfs_fat_sdcard_unmount instead")));
esp_err_t esp_vfs_fat_info(const char* base_path, uint64_t* out_total_bytes, uint64_t* out_free_bytes);
esp_err_t esp_vfs_fat_sdcard_format(const char* base_path, sdmmc_card_t* card);
esp_err_t esp_vfs_fat_sdcard_format_cfg(const char* base_path, sdmmc_card_t* card, esp_vfs_fat_mount_config_t* cfg);
esp_err_t esp_vfs_fat_spiflash_mount_rw_wl(const char* base_path, const char* partition_label, const esp_vfs_fat_mount_config_t* mount_config,
                                           wl_handle_t* wl_handle) CARDENZA_WEB_UNSUPPORTED;
esp_err_t esp_vfs_fat_spiflash_mount_ro(const char* base_path, const char* partition_label,
                                        const esp_vfs_fat_mount_config_t* mount_config) CARDENZA_WEB_UNSUPPORTED;

#ifdef __cplusplus
}
#endif
