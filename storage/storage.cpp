#include "storage.h"
#include "../board/pins.h"

#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>

namespace CardenzaHAL::Storage {

namespace {

bool mounted = false;
uint64_t capacity = 0;
sdmmc_card_t* card = nullptr;

constexpr char kMountPoint[] = "/sdcard";

}  // namespace

bool Init()
{
    if (mounted)
    {
        return true;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slotConfig = SDMMC_SLOT_CONFIG_DEFAULT();
    slotConfig.clk = static_cast<gpio_num_t>(Pins::kSdClk);
    slotConfig.cmd = static_cast<gpio_num_t>(Pins::kSdCmd);
    slotConfig.d0 = static_cast<gpio_num_t>(Pins::kSdDat0);
    slotConfig.d1 = static_cast<gpio_num_t>(Pins::kSdDat1);
    slotConfig.d2 = static_cast<gpio_num_t>(Pins::kSdDat2);
    slotConfig.d3 = static_cast<gpio_num_t>(Pins::kSdDat3);
    slotConfig.width = 4;

    esp_vfs_fat_sdmmc_mount_config_t mountConfig = {};
    mountConfig.format_if_mount_failed = false;
    mountConfig.max_files = 5;
    mountConfig.allocation_unit_size = 16 * 1024;

    esp_err_t status = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slotConfig, &mountConfig, &card);
    if (status != ESP_OK)
    {
        card = nullptr;
        return false;
    }

    mounted = true;
    capacity = static_cast<uint64_t>(card->csd.capacity) * card->csd.sector_size;
    return true;
}

bool IsMounted()
{
    return mounted;
}

uint64_t GetCapacity()
{
    return capacity;
}

}  // namespace CardenzaHAL::Storage
