#pragma once

// ESP-IDF's log: "I (1234) tag: message", at CONFIG_LOG_DEFAULT_LEVEL and per tag. On the web it goes
// to the page's console (stdout); ESP_LOG_BUFFER_* and esp_log_set_vprintf work as on the device.

#include <stdarg.h>
#include <stdint.h>
#include <inttypes.h>

#include "sdkconfig.h"
#include "esp_err.h"

#ifndef CONFIG_LOG_DEFAULT_LEVEL
#define CONFIG_LOG_DEFAULT_LEVEL 3
#endif
#ifndef CONFIG_LOG_MAXIMUM_LEVEL
#define CONFIG_LOG_MAXIMUM_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#endif

CARDENZA_WEB_EXTERN_C_BEGIN

typedef enum {
    ESP_LOG_NONE,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE
} esp_log_level_t;

typedef int (*vprintf_like_t)(const char*, va_list);

void esp_log_set_level_master(esp_log_level_t level);
esp_log_level_t esp_log_get_level_master(void);
void esp_log_level_set(const char* tag, esp_log_level_t level);
esp_log_level_t esp_log_level_get(const char* tag);
vprintf_like_t esp_log_set_vprintf(vprintf_like_t func);
uint32_t esp_log_timestamp(void);
char* esp_log_system_timestamp(void);
uint32_t esp_log_early_timestamp(void);
void esp_log_write(esp_log_level_t level, const char* tag, const char* format, ...) __attribute__((format(printf, 3, 4)));
void esp_log_writev(esp_log_level_t level, const char* tag, const char* format, va_list args);
void esp_log_buffer_hex_internal(const char* tag, const void* buffer, uint16_t buff_len, esp_log_level_t level);
void esp_log_buffer_char_internal(const char* tag, const void* buffer, uint16_t buff_len, esp_log_level_t level);
void esp_log_buffer_hexdump_internal(const char* tag, const void* buffer, uint16_t buff_len, esp_log_level_t log_level);

#ifndef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL CONFIG_LOG_MAXIMUM_LEVEL
#endif

#define LOG_COLOR_E
#define LOG_COLOR_W
#define LOG_COLOR_I
#define LOG_COLOR_D
#define LOG_COLOR_V
#define LOG_RESET_COLOR
#define LOG_FORMAT(letter, format) #letter " (%" PRIu32 ") %s: " format "\n"
#define LOG_SYSTEM_TIME_FORMAT(letter, format) #letter " (%s) %s: " format "\n"

#define ESP_LOG_LEVEL(level, tag, format, ...) do { \
        if (level == ESP_LOG_ERROR) esp_log_write(ESP_LOG_ERROR, tag, LOG_FORMAT(E, format), esp_log_timestamp(), tag, ##__VA_ARGS__); \
        else if (level == ESP_LOG_WARN) esp_log_write(ESP_LOG_WARN, tag, LOG_FORMAT(W, format), esp_log_timestamp(), tag, ##__VA_ARGS__); \
        else if (level == ESP_LOG_DEBUG) esp_log_write(ESP_LOG_DEBUG, tag, LOG_FORMAT(D, format), esp_log_timestamp(), tag, ##__VA_ARGS__); \
        else if (level == ESP_LOG_VERBOSE) esp_log_write(ESP_LOG_VERBOSE, tag, LOG_FORMAT(V, format), esp_log_timestamp(), tag, ##__VA_ARGS__); \
        else esp_log_write(ESP_LOG_INFO, tag, LOG_FORMAT(I, format), esp_log_timestamp(), tag, ##__VA_ARGS__); \
    } while (0)
#define ESP_LOG_LEVEL_LOCAL(level, tag, format, ...) do { if (LOG_LOCAL_LEVEL >= level) ESP_LOG_LEVEL(level, tag, format, ##__VA_ARGS__); } while (0)

#define ESP_LOGE(tag, format, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_ERROR, tag, format, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_WARN, tag, format, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_INFO, tag, format, ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG, tag, format, ##__VA_ARGS__)
#define ESP_LOGV(tag, format, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, tag, format, ##__VA_ARGS__)
#define ESP_EARLY_LOGE ESP_LOGE
#define ESP_EARLY_LOGW ESP_LOGW
#define ESP_EARLY_LOGI ESP_LOGI
#define ESP_EARLY_LOGD ESP_LOGD
#define ESP_EARLY_LOGV ESP_LOGV
#define ESP_DRAM_LOGE ESP_LOGE
#define ESP_DRAM_LOGW ESP_LOGW
#define ESP_DRAM_LOGI ESP_LOGI
#define ESP_DRAM_LOGD ESP_LOGD
#define ESP_DRAM_LOGV ESP_LOGV

#define ESP_LOG_BUFFER_HEX_LEVEL(tag, buffer, buff_len, level) do { if (LOG_LOCAL_LEVEL >= (level)) esp_log_buffer_hex_internal(tag, buffer, buff_len, level); } while (0)
#define ESP_LOG_BUFFER_CHAR_LEVEL(tag, buffer, buff_len, level) do { if (LOG_LOCAL_LEVEL >= (level)) esp_log_buffer_char_internal(tag, buffer, buff_len, level); } while (0)
#define ESP_LOG_BUFFER_HEXDUMP(tag, buffer, buff_len, level) do { if (LOG_LOCAL_LEVEL >= (level)) esp_log_buffer_hexdump_internal(tag, buffer, buff_len, level); } while (0)
#define ESP_LOG_BUFFER_HEX(tag, buffer, buff_len) ESP_LOG_BUFFER_HEX_LEVEL(tag, buffer, buff_len, ESP_LOG_INFO)
#define ESP_LOG_BUFFER_CHAR(tag, buffer, buff_len) ESP_LOG_BUFFER_CHAR_LEVEL(tag, buffer, buff_len, ESP_LOG_INFO)
#define esp_log_buffer_hex ESP_LOG_BUFFER_HEX
#define esp_log_buffer_char ESP_LOG_BUFFER_CHAR

CARDENZA_WEB_EXTERN_C_END
