#pragma once
#include <stdint.h>
#include "esp_err.h"
CARDENZA_WEB_EXTERN_C_BEGIN
typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_WIFI_SOFTAP, ESP_MAC_BT, ESP_MAC_ETH, ESP_MAC_IEEE802154, ESP_MAC_BASE, ESP_MAC_EFUSE_FACTORY, ESP_MAC_EFUSE_CUSTOM, ESP_MAC_EFUSE_EXT } esp_mac_type_t;
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
// A fixed address for every simulator (no two simulators share a bus).
esp_err_t esp_read_mac(uint8_t* mac, esp_mac_type_t type);
esp_err_t esp_efuse_mac_get_default(uint8_t* mac);
esp_err_t esp_base_mac_addr_get(uint8_t* mac);
esp_err_t esp_derive_local_mac(uint8_t* local_mac, const uint8_t* universal_mac);
CARDENZA_WEB_EXTERN_C_END
