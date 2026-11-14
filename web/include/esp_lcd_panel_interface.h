#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct esp_lcd_panel_t esp_lcd_panel_t;
struct esp_lcd_panel_t {
    esp_err_t (*reset)(esp_lcd_panel_t* panel);
    esp_err_t (*init)(esp_lcd_panel_t* panel);
    esp_err_t (*del)(esp_lcd_panel_t* panel);
    esp_err_t (*draw_bitmap)(esp_lcd_panel_t* panel, int x_start, int y_start, int x_end, int y_end, const void* color_data);
    esp_err_t (*mirror)(esp_lcd_panel_t* panel, bool x_axis, bool y_axis);
    esp_err_t (*swap_xy)(esp_lcd_panel_t* panel, bool swap_axes);
    esp_err_t (*set_gap)(esp_lcd_panel_t* panel, int x_gap, int y_gap);
    esp_err_t (*invert_color)(esp_lcd_panel_t* panel, bool invert_color_data);
    esp_err_t (*disp_on_off)(esp_lcd_panel_t* panel, bool on_off);
    esp_err_t (*disp_sleep)(esp_lcd_panel_t* panel, bool sleep);
    void* user_data;
};
#ifdef __cplusplus
}
#endif
