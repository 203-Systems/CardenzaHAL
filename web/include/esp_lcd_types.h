#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hal/lcd_types.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct esp_lcd_panel_io_t* esp_lcd_panel_io_handle_t;
typedef struct esp_lcd_panel_t* esp_lcd_panel_handle_t;
typedef enum { LCD_RGB_ELEMENT_ORDER_RGB = COLOR_RGB_ELEMENT_ORDER_RGB, LCD_RGB_ELEMENT_ORDER_BGR = COLOR_RGB_ELEMENT_ORDER_BGR } lcd_rgb_element_order_t;
typedef lcd_rgb_element_order_t lcd_color_rgb_endian_t;
#define LCD_RGB_ENDIAN_RGB (lcd_color_rgb_endian_t) LCD_RGB_ELEMENT_ORDER_RGB
#define LCD_RGB_ENDIAN_BGR (lcd_color_rgb_endian_t) LCD_RGB_ELEMENT_ORDER_BGR
typedef lcd_rgb_element_order_t esp_lcd_color_space_t;
#define ESP_LCD_COLOR_SPACE_RGB (esp_lcd_color_space_t) LCD_RGB_ELEMENT_ORDER_RGB
#define ESP_LCD_COLOR_SPACE_BGR (esp_lcd_color_space_t) LCD_RGB_ELEMENT_ORDER_BGR
#define ESP_LCD_COLOR_SPACE_MONOCHROME (esp_lcd_color_space_t)2
// Empty, as in ESP-IDF (clang warns that C and C++ size it differently; it is never copied across).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextern-c-compat"
typedef struct {
} esp_lcd_panel_io_event_data_t;
#pragma clang diagnostic pop
typedef bool (*esp_lcd_panel_io_color_trans_done_cb_t)(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t* edata, void* user_ctx);
typedef struct {
    esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
} esp_lcd_panel_io_callbacks_t;
#ifdef __cplusplus
}
#endif
