#include "storage.h"
#include "../board/pins.h"

#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <esp_log.h>
#include <mutex>

namespace CardenzaHAL::Storage {

namespace {

bool mounted = false;
uint64_t capacity = 0;
sdmmc_card_t* card = nullptr;
std::mutex stateMutex;

constexpr char kMountPoint[] = "/sdcard";

}  // namespace

bool Init()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    if (mounted)
    {
        return true;
    }

    if (card != nullptr)
    {
        // Files are closed by the failed operation. Defer teardown to the
        // consumer's mount retry, never from the middle of an active write.
        sdmmc_card_t* stale = card;
        card = nullptr; // The SDK frees the card even if VFS unregister fails.
        if (esp_vfs_fat_sdcard_unmount(kMountPoint, stale) != ESP_OK) return false;
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
    std::lock_guard<std::mutex> lock(stateMutex);
    return mounted;
}

bool CheckAfterIoError()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    if (!mounted) return false;
    const esp_err_t status = sdmmc_get_status(card);
    if (status != ESP_OK)
    {
        ESP_LOGW("CardenzaStorage", "card stopped responding after I/O error: %s", esp_err_to_name(status));
        mounted = false;
        capacity = 0;
    }
    return mounted;
}

uint64_t GetCapacity()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return capacity;
}

}  // namespace CardenzaHAL::Storage
