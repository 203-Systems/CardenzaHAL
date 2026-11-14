#pragma once

// I2S on the web target. A TX channel is an emulated DMA queue (dma_desc_num buffers of
// dma_frame_num frames) drained at the sample rate by the page's audio clock, which plays what it
// drains; i2s_channel_write blocks while the queue is full, like on the device. An RX channel fills at
// the sample rate with the page's microphone, or silence. Callbacks (on_sent, on_send_q_ovf, on_recv,
// on_recv_q_ovf) run in ISR context at the same moments.

#include "driver/i2s_types.h"
#include "hal/i2s_types.h"
#include "hal/gpio_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define I2S_CHANNEL_DEFAULT_CONFIG(i2s_num, i2s_role) { \
    .id = i2s_num, \
    .role = i2s_role, \
    .dma_desc_num = 6, \
    .dma_frame_num = 240, \
    .auto_clear_after_cb = false, \
    .auto_clear_before_cb = false, \
    .intr_priority = 0, \
}
#define I2S_GPIO_UNUSED GPIO_NUM_NC

typedef struct {
    i2s_isr_callback_t on_recv;
    i2s_isr_callback_t on_recv_q_ovf;
    i2s_isr_callback_t on_sent;
    i2s_isr_callback_t on_send_q_ovf;
} i2s_event_callbacks_t;

typedef struct {
    i2s_port_t id;
    i2s_role_t role;
    uint32_t dma_desc_num;
    uint32_t dma_frame_num;
    union {
        bool auto_clear;
        bool auto_clear_after_cb;
    };
    bool auto_clear_before_cb;
    int intr_priority;
} i2s_chan_config_t;

typedef struct {
    i2s_port_t id;
    i2s_role_t role;
    i2s_dir_t dir;
    i2s_comm_mode_t mode;
    i2s_chan_handle_t pair_chan;
    uint32_t total_dma_buf_size;
} i2s_chan_info_t;

esp_err_t i2s_new_channel(const i2s_chan_config_t* chan_cfg, i2s_chan_handle_t* ret_tx_handle, i2s_chan_handle_t* ret_rx_handle);
esp_err_t i2s_del_channel(i2s_chan_handle_t handle);
esp_err_t i2s_channel_get_info(i2s_chan_handle_t handle, i2s_chan_info_t* chan_info);
esp_err_t i2s_channel_enable(i2s_chan_handle_t handle);
esp_err_t i2s_channel_disable(i2s_chan_handle_t handle);
esp_err_t i2s_channel_preload_data(i2s_chan_handle_t tx_handle, const void* src, size_t size, size_t* bytes_loaded);
esp_err_t i2s_channel_write(i2s_chan_handle_t handle, const void* src, size_t size, size_t* bytes_written, uint32_t timeout_ms);
esp_err_t i2s_channel_read(i2s_chan_handle_t handle, void* dest, size_t size, size_t* bytes_read, uint32_t timeout_ms);
esp_err_t i2s_channel_register_event_callback(i2s_chan_handle_t handle, const i2s_event_callbacks_t* callbacks, void* user_data);

#ifdef __cplusplus
}
#endif
