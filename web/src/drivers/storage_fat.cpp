// The SD card on the web target: the page's /sdcard (Emscripten's file system, kept in IndexedDB
// when the page may). esp_vfs_fat_sdmmc_mount succeeds when the page gave this run a card and fails
// like an empty slot (ESP_ERR_TIMEOUT) when it did not; the HAL's Storage::Init runs on it unchanged.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "../platform.h"

namespace p = cardenza_web::platform;

namespace {

constexpr int kMaxMounts = 2;
struct Mount {
    char base[32];
    sdmmc_card_t* card;
};
Mount g_mounts[kMaxMounts];

Mount* Find(const char* base)
{
    for (Mount& mount : g_mounts)
        if (mount.card != nullptr && std::strcmp(mount.base, base) == 0) return &mount;
    return nullptr;
}

uint64_t UsedBytes(const std::string& path, int depth = 0)
{
    if (depth > 16) return 0;
    DIR* directory = opendir(path.c_str());
    if (directory == nullptr) return 0;
    uint64_t total = 0;
    while (dirent* entry = readdir(directory))
    {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        const std::string child = path + "/" + entry->d_name;
        struct stat info;
        if (stat(child.c_str(), &info) != 0) continue;
        if (S_ISDIR(info.st_mode)) total += 16384 + UsedBytes(child, depth + 1);
        else total += (static_cast<uint64_t>(info.st_size) + 16383) / 16384 * 16384;  // whole clusters
    }
    closedir(directory);
    return total;
}

}  // namespace

extern "C" {

esp_err_t sdmmc_host_init(void) { return ESP_OK; }
esp_err_t sdmmc_host_deinit(void) { return ESP_OK; }
esp_err_t sdmmc_host_set_bus_width(int, size_t) { return ESP_OK; }
size_t sdmmc_host_get_slot_width(int) { return 4; }
esp_err_t sdmmc_host_set_bus_ddr_mode(int, bool) { return ESP_OK; }
esp_err_t sdmmc_host_set_card_clk(int, uint32_t) { return ESP_OK; }
esp_err_t sdmmc_host_set_cclk_always_on(int, bool) { return ESP_OK; }
esp_err_t sdmmc_host_do_transaction(int, sdmmc_command_t*) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sdmmc_host_io_int_enable(int) { return ESP_OK; }
esp_err_t sdmmc_host_io_int_wait(int, TickType_t) { return ESP_ERR_TIMEOUT; }

esp_err_t sdmmc_host_get_real_freq(int, int* real_freq_khz)
{
    if (real_freq_khz != nullptr) *real_freq_khz = SDMMC_FREQ_DEFAULT;
    return ESP_OK;
}

esp_err_t sdmmc_host_set_input_delay(int, sdmmc_delay_phase_t) { return ESP_OK; }

esp_err_t esp_vfs_fat_sdmmc_mount(const char* base_path, const sdmmc_host_t* host_config, const void*,
                                  const esp_vfs_fat_mount_config_t*, sdmmc_card_t** out_card)
{
    if (base_path == nullptr || host_config == nullptr) return ESP_ERR_INVALID_ARG;
    if (Find(base_path) != nullptr) return ESP_ERR_INVALID_STATE;
    // Probing the slot takes a moment on the device too.
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) vTaskDelay(pdMS_TO_TICKS(20));
    if (!p::CardPresent())
    {
        ESP_LOGE("sdmmc_common", "sdmmc_init_ocr: send_op_cond (1) returned 0x107");
        return ESP_ERR_TIMEOUT;
    }
    Mount* slot = nullptr;
    for (Mount& mount : g_mounts)
        if (mount.card == nullptr) slot = &mount;
    if (slot == nullptr) return ESP_ERR_NO_MEM;
    mkdir(base_path, 0777);
    auto* card = static_cast<sdmmc_card_t*>(std::calloc(1, sizeof(sdmmc_card_t)));
    if (card == nullptr) return ESP_ERR_NO_MEM;
    card->host = *host_config;
    card->csd.capacity = static_cast<int>(p::CardBytes() / 512);
    card->csd.sector_size = 512;
    card->csd.read_block_len = 512;
    std::strncpy(card->cid.name, "WEBSD", sizeof(card->cid.name));
    card->is_mem = 1;
    card->max_freq_khz = SDMMC_FREQ_DEFAULT;
    card->real_freq_khz = SDMMC_FREQ_DEFAULT;
    card->log_bus_width = 2;
    std::snprintf(slot->base, sizeof(slot->base), "%s", base_path);
    slot->card = card;
    if (out_card != nullptr) *out_card = card;
    return ESP_OK;
}

esp_err_t esp_vfs_fat_sdcard_unmount(const char* base_path, sdmmc_card_t* card)
{
    Mount* mount = base_path != nullptr ? Find(base_path) : nullptr;
    if (mount == nullptr || (card != nullptr && mount->card != card)) return ESP_ERR_INVALID_ARG;
    std::free(mount->card);
    mount->card = nullptr;
    p::FilesChanged();
    return ESP_OK;
}

esp_err_t esp_vfs_fat_sdmmc_unmount(void)
{
    for (Mount& mount : g_mounts)
        if (mount.card != nullptr) return esp_vfs_fat_sdcard_unmount(mount.base, mount.card);
    return ESP_ERR_INVALID_STATE;
}

esp_err_t esp_vfs_fat_info(const char* base_path, uint64_t* out_total_bytes, uint64_t* out_free_bytes)
{
    if (base_path == nullptr || Find(base_path) == nullptr) return ESP_ERR_INVALID_STATE;
    const uint64_t total = p::CardBytes();
    const uint64_t used = UsedBytes(base_path);
    if (out_total_bytes != nullptr) *out_total_bytes = total;
    if (out_free_bytes != nullptr) *out_free_bytes = used >= total ? 0 : total - used;
    return ESP_OK;
}

esp_err_t esp_vfs_fat_sdcard_format(const char*, sdmmc_card_t*) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_vfs_fat_sdcard_format_cfg(const char*, sdmmc_card_t*, esp_vfs_fat_mount_config_t*) { return ESP_ERR_NOT_SUPPORTED; }

esp_err_t sdmmc_get_status(sdmmc_card_t* card)
{
    if (card == nullptr) return ESP_ERR_INVALID_ARG;
    return p::CardPresent() ? ESP_OK : ESP_ERR_TIMEOUT;
}

void sdmmc_card_print_info(FILE* stream, const sdmmc_card_t* card)
{
    if (card == nullptr) return;
    std::fprintf(stream, "Name: %s\nType: SDHC/SDXC\nSpeed: default speed\nSize: %lluMB\n", card->cid.name,
                 static_cast<unsigned long long>(card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
}

}  // extern "C"
