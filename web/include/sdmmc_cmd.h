#pragma once
#include <stdio.h>
#include "esp_err.h"
#include "driver/sdmmc_types.h"
#ifdef __cplusplus
extern "C" {
#endif
void sdmmc_card_print_info(FILE* stream, const sdmmc_card_t* card);
// ESP_OK while the page's card is there.
esp_err_t sdmmc_get_status(sdmmc_card_t* card);
esp_err_t sdmmc_card_init(const sdmmc_host_t* host, sdmmc_card_t* out_card) CARDENZA_WEB_UNSUPPORTED;
esp_err_t sdmmc_write_sectors(sdmmc_card_t* card, const void* src, size_t start_sector, size_t sector_count) CARDENZA_WEB_UNSUPPORTED;
esp_err_t sdmmc_read_sectors(sdmmc_card_t* card, void* dst, size_t start_sector, size_t sector_count) CARDENZA_WEB_UNSUPPORTED;
#ifdef __cplusplus
}
#endif
