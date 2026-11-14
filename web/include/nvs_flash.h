#pragma once
#include "nvs.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_init_partition(const char* partition_label);
esp_err_t nvs_flash_deinit(void);
esp_err_t nvs_flash_deinit_partition(const char* partition_label);
esp_err_t nvs_flash_erase(void);
esp_err_t nvs_flash_erase_partition(const char* part_name);
#ifdef __cplusplus
}
#endif
