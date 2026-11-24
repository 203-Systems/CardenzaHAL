#include "storage.h"
#include "../system/internal/hooks.h"
#include "internal/lend.h"
#include "internal/sd_slot.h"

#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <esp_log.h>
#include <mutex>

namespace CardenzaHAL::Storage {

namespace {

bool mounted = false;
bool lent = false;  // to a computer, as a USB drive (internal/lend.h)
uint64_t capacity = 0;
sdmmc_card_t* card = nullptr;
std::mutex stateMutex;

// The file system lets the card go (with stateMutex held).
bool UnmountLocked()
{
    sdmmc_card_t* stale = card;
    card = nullptr;  // The SDK frees the card even if VFS unregister fails.
    const bool unmounted = esp_vfs_fat_sdcard_unmount(kMountPoint, stale) == ESP_OK;
    System::detail::RestoreHomeButton();  // the SD driver's slot reset takes GPIO0 too
    return unmounted;
}

// With stateMutex held.
bool InitLocked()
{
    if (lent) return false;  // a computer has it (USB::Drive)
    if (mounted)
    {
        return true;
    }

    if (card != nullptr)
    {
        // Files are closed by the failed operation. Defer teardown to the
        // consumer's mount retry, never from the middle of an active write.
        if (!UnmountLocked()) return false;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slotConfig = detail::SlotConfig();

    esp_vfs_fat_sdmmc_mount_config_t mountConfig = {};
    mountConfig.format_if_mount_failed = false;
    mountConfig.max_files = 5;
    mountConfig.allocation_unit_size = 16 * 1024;

    esp_err_t status = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slotConfig, &mountConfig, &card);
    if (status != ESP_OK)
    {
        card = nullptr;
        // ESP-IDF 5.3's SD driver resets every pin of the slot it gives up, the unused d4-d7 of a 4-bit
        // slot included, which it keeps as 0: GPIO0, HOME, loses its input (it then always reads pressed,
        // and the Launcher's bootloader takes it for its entry key). Seen on the board 2026-11-17, no card.
        System::detail::RestoreHomeButton();
        return false;
    }

    mounted = true;
    capacity = static_cast<uint64_t>(card->csd.capacity) * card->csd.sector_size;
    return true;
}

}  // namespace

bool Init()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return InitLocked();
}

bool IsMounted()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return mounted;
}

bool LentToComputer()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return lent;
}

namespace detail {

bool Lend()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    if (lent) return true;
    // A mounted card, or one a failed operation left behind (CheckAfterIoError): the file system lets it go, and
    // the SD host with it (the drive starts the host again for the card alone).
    if (card != nullptr && !UnmountLocked())
    {
        mounted = false;
        return false;
    }
    mounted = false;
    lent = true;
    return true;
}

bool GiveBack()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    lent = false;
    return InitLocked();
}

}  // namespace detail

bool CheckAfterIoError()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    if (!mounted || lent) return false;
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
