#pragma once
// The ESP-IDF API the web target follows (ESP-IDF 5.3, what CardenzaHAL requires).
#define ESP_IDF_VERSION_MAJOR 5
#define ESP_IDF_VERSION_MINOR 3
#define ESP_IDF_VERSION_PATCH 5
#define ESP_IDF_VERSION_VAL(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH)
#ifndef IDF_VER
#define IDF_VER "v5.3.5-cardenza-web"
#endif
#ifdef __cplusplus
extern "C" {
#endif
const char* esp_get_idf_version(void);
#ifdef __cplusplus
}
#endif
