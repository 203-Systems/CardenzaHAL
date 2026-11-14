#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_types.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t panel);
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t panel);
esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t panel);
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x_start, int y_start, int x_end, int y_end, const void* color_data);
esp_err_t esp_lcd_panel_mirror(esp_lcd_panel_handle_t panel, bool mirror_x, bool mirror_y);
esp_err_t esp_lcd_panel_swap_xy(esp_lcd_panel_handle_t panel, bool swap_axes);
esp_err_t esp_lcd_panel_set_gap(esp_lcd_panel_handle_t panel, int x_gap, int y_gap);
esp_err_t esp_lcd_panel_invert_color(esp_lcd_panel_handle_t panel, bool invert_color_data);
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t panel, bool on_off);
esp_err_t esp_lcd_panel_disp_off(esp_lcd_panel_handle_t panel, bool off) __attribute__((deprecated("use esp_lcd_panel_disp_on_off instead")));
esp_err_t esp_lcd_panel_disp_sleep(esp_lcd_panel_handle_t panel, bool sleep);
#ifdef __cplusplus
}
#endif
