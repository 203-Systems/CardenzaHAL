#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "esp_bit_defs.h"
#include "soc/soc_caps.h"
#include "soc/clk_tree_defs.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { I2S_SLOT_MODE_MONO = 1, I2S_SLOT_MODE_STEREO = 2 } i2s_slot_mode_t;
typedef enum { I2S_DIR_RX = BIT(0), I2S_DIR_TX = BIT(1) } i2s_dir_t;
typedef enum { I2S_ROLE_MASTER, I2S_ROLE_SLAVE } i2s_role_t;
typedef enum { I2S_DATA_BIT_WIDTH_8BIT = 8, I2S_DATA_BIT_WIDTH_16BIT = 16, I2S_DATA_BIT_WIDTH_24BIT = 24, I2S_DATA_BIT_WIDTH_32BIT = 32 } i2s_data_bit_width_t;
typedef enum { I2S_SLOT_BIT_WIDTH_AUTO = 0, I2S_SLOT_BIT_WIDTH_8BIT = 8, I2S_SLOT_BIT_WIDTH_16BIT = 16, I2S_SLOT_BIT_WIDTH_24BIT = 24,
               I2S_SLOT_BIT_WIDTH_32BIT = 32 } i2s_slot_bit_width_t;
typedef soc_periph_i2s_clk_src_t i2s_clock_src_t;
typedef enum { I2S_PCM_DISABLE = 0, I2S_PCM_A_DECOMPRESS, I2S_PCM_A_COMPRESS, I2S_PCM_U_DECOMPRESS, I2S_PCM_U_COMPRESS } i2s_pcm_compress_t;
typedef enum { I2S_PDM_DSR_8S = 0, I2S_PDM_DSR_16S, I2S_PDM_DSR_MAX } i2s_pdm_dsr_t;
typedef enum { I2S_PDM_SIG_SCALING_DIV_2 = 0, I2S_PDM_SIG_SCALING_MUL_1 = 1, I2S_PDM_SIG_SCALING_MUL_2 = 2, I2S_PDM_SIG_SCALING_MUL_4 = 3 } i2s_pdm_sig_scale_t;
typedef enum { I2S_PDM_TX_ONE_LINE_CODEC, I2S_PDM_TX_ONE_LINE_DAC, I2S_PDM_TX_TWO_LINE_DAC } i2s_pdm_tx_line_mode_t;
typedef enum { I2S_STD_SLOT_LEFT = BIT(0), I2S_STD_SLOT_RIGHT = BIT(1), I2S_STD_SLOT_BOTH = BIT(0) | BIT(1) } i2s_std_slot_mask_t;
typedef enum {
    I2S_PDM_SLOT_RIGHT = BIT(0), I2S_PDM_SLOT_LEFT = BIT(1), I2S_PDM_SLOT_BOTH = BIT(0) | BIT(1),
    I2S_PDM_RX_LINE0_SLOT_RIGHT = I2S_PDM_SLOT_RIGHT, I2S_PDM_RX_LINE0_SLOT_LEFT = I2S_PDM_SLOT_LEFT,
    I2S_PDM_RX_LINE1_SLOT_RIGHT = BIT(2), I2S_PDM_RX_LINE1_SLOT_LEFT = BIT(3), I2S_PDM_RX_LINE2_SLOT_RIGHT = BIT(4),
    I2S_PDM_RX_LINE2_SLOT_LEFT = BIT(5), I2S_PDM_RX_LINE3_SLOT_RIGHT = BIT(6), I2S_PDM_RX_LINE3_SLOT_LEFT = BIT(7),
    I2S_PDM_LINE_SLOT_ALL = 0x00ff,
} i2s_pdm_slot_mask_t;
typedef enum { I2S_TDM_SLOT0 = BIT(0), I2S_TDM_SLOT1 = BIT(1), I2S_TDM_SLOT2 = BIT(2), I2S_TDM_SLOT3 = BIT(3), I2S_TDM_SLOT4 = BIT(4),
               I2S_TDM_SLOT5 = BIT(5), I2S_TDM_SLOT6 = BIT(6), I2S_TDM_SLOT7 = BIT(7) } i2s_tdm_slot_mask_t;
#ifdef __cplusplus
}
#endif
