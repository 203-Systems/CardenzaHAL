#pragma once
#include <stdint.h>
#include "esp_bit_defs.h"
#include "web_compat.h"
CARDENZA_WEB_EXTERN_C_BEGIN
typedef enum { CHIP_ESP32 = 1, CHIP_ESP32S2 = 2, CHIP_ESP32S3 = 9, CHIP_ESP32C3 = 5, CHIP_ESP32C2 = 12, CHIP_ESP32C6 = 13, CHIP_ESP32H2 = 16, CHIP_ESP32P4 = 18, CHIP_POSIX_LINUX = 999 } esp_chip_model_t;
#define CHIP_FEATURE_EMB_FLASH BIT(0)
#define CHIP_FEATURE_WIFI_BGN BIT(1)
#define CHIP_FEATURE_BLE BIT(4)
#define CHIP_FEATURE_BT BIT(5)
#define CHIP_FEATURE_IEEE802154 BIT(6)
#define CHIP_FEATURE_EMB_PSRAM BIT(7)
typedef struct {
    esp_chip_model_t model;
    uint32_t features;
    uint16_t revision;
    uint8_t cores;
} esp_chip_info_t;
void esp_chip_info(esp_chip_info_t* out_info);
CARDENZA_WEB_EXTERN_C_END
