#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_types.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct esp_lcd_panel_io_t esp_lcd_panel_io_t;
struct esp_lcd_panel_io_t {
    esp_err_t (*rx_param)(esp_lcd_panel_io_t* io, int lcd_cmd, void* param, size_t param_size);
    esp_err_t (*tx_param)(esp_lcd_panel_io_t* io, int lcd_cmd, const void* param, size_t param_size);
    esp_err_t (*tx_color)(esp_lcd_panel_io_t* io, int lcd_cmd, const void* color, size_t color_size);
    esp_err_t (*del)(esp_lcd_panel_io_t* io);
    esp_err_t (*register_event_callbacks)(esp_lcd_panel_io_t* io, const esp_lcd_panel_io_callbacks_t* cbs, void* user_ctx);
};
#ifdef __cplusplus
}
#endif
