#pragma once

#include "hal/i2s_types.h"
#include "hal/gpio_types.h"
#include "driver/i2s_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define I2S_PDM_RX_SLOT_DEFAULT_CONFIG(bits_per_sample, mono_or_stereo) { \
    .data_bit_width = bits_per_sample, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO, .slot_mode = mono_or_stereo, \
    .slot_mask = (mono_or_stereo == I2S_SLOT_MODE_MONO) ? I2S_PDM_SLOT_LEFT : I2S_PDM_SLOT_BOTH }
#define I2S_PDM_RX_CLK_DEFAULT_CONFIG(rate) { \
    .sample_rate_hz = rate, .clk_src = I2S_CLK_SRC_DEFAULT, .mclk_multiple = I2S_MCLK_MULTIPLE_256, \
    .dn_sample_mode = I2S_PDM_DSR_8S, .bclk_div = 8 }

typedef struct {
    i2s_data_bit_width_t data_bit_width;
    i2s_slot_bit_width_t slot_bit_width;
    i2s_slot_mode_t slot_mode;
    i2s_pdm_slot_mask_t slot_mask;
} i2s_pdm_rx_slot_config_t;

typedef struct {
    uint32_t sample_rate_hz;
    i2s_clock_src_t clk_src;
    i2s_mclk_multiple_t mclk_multiple;
    i2s_pdm_dsr_t dn_sample_mode;
    uint32_t bclk_div;
} i2s_pdm_rx_clk_config_t;

typedef struct {
    gpio_num_t clk;
    union {
        gpio_num_t din;
        gpio_num_t dins[SOC_I2S_PDM_MAX_RX_LINES];
    };
    struct {
        uint32_t clk_inv : 1;
    } invert_flags;
} i2s_pdm_rx_gpio_config_t;

typedef struct {
    i2s_pdm_rx_clk_config_t clk_cfg;
    i2s_pdm_rx_slot_config_t slot_cfg;
    i2s_pdm_rx_gpio_config_t gpio_cfg;
} i2s_pdm_rx_config_t;

esp_err_t i2s_channel_init_pdm_rx_mode(i2s_chan_handle_t handle, const i2s_pdm_rx_config_t* pdm_rx_cfg);
esp_err_t i2s_channel_reconfig_pdm_rx_clock(i2s_chan_handle_t handle, const i2s_pdm_rx_clk_config_t* clk_cfg);
esp_err_t i2s_channel_reconfig_pdm_rx_slot(i2s_chan_handle_t handle, const i2s_pdm_rx_slot_config_t* slot_cfg);
esp_err_t i2s_channel_reconfig_pdm_rx_gpio(i2s_chan_handle_t handle, const i2s_pdm_rx_gpio_config_t* gpio_cfg);

#ifdef __cplusplus
}
#endif
