#pragma once
#include "esp_err.h"
#include "esp_log.h"

#define ESP_RETURN_ON_ERROR(x, log_tag, format, ...) do { esp_err_t err_rc_ = (x); \
        if (unlikely(err_rc_ != ESP_OK)) { ESP_LOGE(log_tag, "%s(%d): " format, __FUNCTION__, __LINE__, ##__VA_ARGS__); return err_rc_; } } while (0)
#define ESP_RETURN_ON_ERROR_ISR ESP_RETURN_ON_ERROR
#define ESP_GOTO_ON_ERROR(x, goto_tag, log_tag, format, ...) do { esp_err_t err_rc_ = (x); \
        if (unlikely(err_rc_ != ESP_OK)) { ESP_LOGE(log_tag, "%s(%d): " format, __FUNCTION__, __LINE__, ##__VA_ARGS__); ret = err_rc_; goto goto_tag; } } while (0)
#define ESP_GOTO_ON_ERROR_ISR ESP_GOTO_ON_ERROR
#define ESP_RETURN_ON_FALSE(a, err_code, log_tag, format, ...) do { \
        if (unlikely(!(a))) { ESP_LOGE(log_tag, "%s(%d): " format, __FUNCTION__, __LINE__, ##__VA_ARGS__); return err_code; } } while (0)
#define ESP_RETURN_ON_FALSE_ISR ESP_RETURN_ON_FALSE
#define ESP_GOTO_ON_FALSE(a, err_code, goto_tag, log_tag, format, ...) do { \
        if (unlikely(!(a))) { ESP_LOGE(log_tag, "%s(%d): " format, __FUNCTION__, __LINE__, ##__VA_ARGS__); ret = err_code; goto goto_tag; } } while (0)
#define ESP_GOTO_ON_FALSE_ISR ESP_GOTO_ON_FALSE
#define ESP_RETURN_VOID_ON_ERROR(x, log_tag, format, ...) do { esp_err_t err_rc_ = (x); \
        if (unlikely(err_rc_ != ESP_OK)) { ESP_LOGE(log_tag, "%s(%d): " format, __FUNCTION__, __LINE__, ##__VA_ARGS__); return; } } while (0)
#define ESP_RETURN_VOID_ON_FALSE(a, log_tag, format, ...) do { \
        if (unlikely(!(a))) { ESP_LOGE(log_tag, "%s(%d): " format, __FUNCTION__, __LINE__, ##__VA_ARGS__); return; } } while (0)
