#pragma once

// The panel IO of the display (Display::GetPanelIO): commands and pixel data go to the web target's
// ST7789 model, with the SPI time a transfer takes on the device (40 MHz); on_color_trans_done runs in
// ISR context when a transfer is done.

#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_io_spi.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_lcd_panel_io_rx_param(esp_lcd_panel_io_handle_t io, int lcd_cmd, void* param, size_t param_size);
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int lcd_cmd, const void* param, size_t param_size);
esp_err_t esp_lcd_panel_io_tx_color(esp_lcd_panel_io_handle_t io, int lcd_cmd, const void* color, size_t color_size);
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io);
esp_err_t esp_lcd_panel_io_register_event_callbacks(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_io_callbacks_t* cbs, void* user_ctx);
#ifdef __cplusplus
}
#endif
