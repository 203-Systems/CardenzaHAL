#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "soc/soc_caps.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { I2S_NUM_0 = 0, I2S_NUM_1 = 1, I2S_NUM_AUTO } i2s_port_t;
typedef enum { I2S_COMM_MODE_STD, I2S_COMM_MODE_PDM, I2S_COMM_MODE_TDM, I2S_COMM_MODE_NONE } i2s_comm_mode_t;
typedef enum { I2S_MCLK_MULTIPLE_128 = 128, I2S_MCLK_MULTIPLE_192 = 192, I2S_MCLK_MULTIPLE_256 = 256, I2S_MCLK_MULTIPLE_384 = 384,
               I2S_MCLK_MULTIPLE_512 = 512, I2S_MCLK_MULTIPLE_576 = 576, I2S_MCLK_MULTIPLE_768 = 768, I2S_MCLK_MULTIPLE_1024 = 1024,
               I2S_MCLK_MULTIPLE_1152 = 1152 } i2s_mclk_multiple_t;
typedef struct {
    void* data __attribute__((deprecated));
    void* dma_buf;
    size_t size;
} i2s_event_data_t;
typedef struct i2s_channel_obj_t* i2s_chan_handle_t;
typedef bool (*i2s_isr_callback_t)(i2s_chan_handle_t handle, i2s_event_data_t* event, void* user_ctx);
#ifdef __cplusplus
}
#endif
