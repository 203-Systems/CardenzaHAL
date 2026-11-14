#pragma once

#include "hal/i2s_types.h"
#include "hal/gpio_types.h"
#include "driver/i2s_common.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits_per_sample, mono_or_stereo) { \
    .data_bit_width = bits_per_sample, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO, .slot_mode = mono_or_stereo, \
    .slot_mask = I2S_STD_SLOT_BOTH, .ws_width = bits_per_sample, .ws_pol = false, .bit_shift = true, \
    .left_align = true, .big_endian = false, .bit_order_lsb = false }
#define I2S_STD_PCM_SLOT_DEFAULT_CONFIG(bits_per_sample, mono_or_stereo) { \
    .data_bit_width = bits_per_sample, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO, .slot_mode = mono_or_stereo, \
    .slot_mask = I2S_STD_SLOT_BOTH, .ws_width = 1, .ws_pol = true, .bit_shift = true, \
    .left_align = true, .big_endian = false, .bit_order_lsb = false }
#define I2S_STD_MSB_SLOT_DEFAULT_CONFIG(bits_per_sample, mono_or_stereo) { \
    .data_bit_width = bits_per_sample, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO, .slot_mode = mono_or_stereo, \
    .slot_mask = I2S_STD_SLOT_BOTH, .ws_width = bits_per_sample, .ws_pol = false, .bit_shift = false, \
    .left_align = true, .big_endian = false, .bit_order_lsb = false }
#define I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(bits_per_sample, mono_or_stereo) I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits_per_sample, mono_or_stereo)
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) { \
    .sample_rate_hz = rate, .clk_src = I2S_CLK_SRC_DEFAULT, .ext_clk_freq_hz = 0, .mclk_multiple = I2S_MCLK_MULTIPLE_256 }

typedef struct {
    i2s_data_bit_width_t data_bit_width;
    i2s_slot_bit_width_t slot_bit_width;
    i2s_slot_mode_t slot_mode;
    i2s_std_slot_mask_t slot_mask;
    uint32_t ws_width;
    bool ws_pol;
    bool bit_shift;
    bool left_align;
    bool big_endian;
    bool bit_order_lsb;
} i2s_std_slot_config_t;

typedef struct {
    uint32_t sample_rate_hz;
    i2s_clock_src_t clk_src;
    uint32_t ext_clk_freq_hz;
    i2s_mclk_multiple_t mclk_multiple;
} i2s_std_clk_config_t;

typedef struct {
    gpio_num_t mclk;
    gpio_num_t bclk;
    gpio_num_t ws;
    gpio_num_t dout;
    gpio_num_t din;
    struct {
        uint32_t mclk_inv : 1;
        uint32_t bclk_inv : 1;
        uint32_t ws_inv : 1;
    } invert_flags;
} i2s_std_gpio_config_t;

typedef struct {
    i2s_std_clk_config_t clk_cfg;
    i2s_std_slot_config_t slot_cfg;
    i2s_std_gpio_config_t gpio_cfg;
} i2s_std_config_t;

esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t* std_cfg);
esp_err_t i2s_channel_reconfig_std_clock(i2s_chan_handle_t handle, const i2s_std_clk_config_t* clk_cfg);
esp_err_t i2s_channel_reconfig_std_slot(i2s_chan_handle_t handle, const i2s_std_slot_config_t* slot_cfg);
esp_err_t i2s_channel_reconfig_std_gpio(i2s_chan_handle_t handle, const i2s_std_gpio_config_t* gpio_cfg);

#ifdef __cplusplus
}
#endif
