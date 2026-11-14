#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "hal/spi_types.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef int esp_lcd_spi_bus_handle_t;
typedef struct {
    int cs_gpio_num;
    int dc_gpio_num;
    int spi_mode;
    unsigned int pclk_hz;
    size_t trans_queue_depth;
    esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
    void* user_ctx;
    int lcd_cmd_bits;
    int lcd_param_bits;
    uint8_t cs_ena_pretrans;
    uint8_t cs_ena_posttrans;
    struct {
        unsigned int dc_high_on_cmd : 1;
        unsigned int dc_low_on_data : 1;
        unsigned int dc_low_on_param : 1;
        unsigned int octal_mode : 1;
        unsigned int quad_mode : 1;
        unsigned int sio_mode : 1;
        unsigned int lsb_first : 1;
        unsigned int cs_high_active : 1;
    } flags;
} esp_lcd_panel_io_spi_config_t;
// The display's bus and panel IO belong to the HAL (Display::GetPanelIO): a firmware does not make its own.
esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus, const esp_lcd_panel_io_spi_config_t* io_config,
                                   esp_lcd_panel_io_handle_t* ret_io) CARDENZA_WEB_HAL_ONLY;
#ifdef __cplusplus
}
#endif
