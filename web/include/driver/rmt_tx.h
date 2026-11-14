#pragma once

// RMT transmit, for the IR LED (GPIO44, IR::Send). On the web target a transmission takes its real
// time and its symbols are decoded as NEC for the page (it shows what was sent).

#include "esp_err.h"
#include "driver/rmt_common.h"
#include "driver/rmt_encoder.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    rmt_tx_done_callback_t on_trans_done;
} rmt_tx_event_callbacks_t;
typedef struct {
    gpio_num_t gpio_num;
    rmt_clock_source_t clk_src;
    uint32_t resolution_hz;
    size_t mem_block_symbols;
    size_t trans_queue_depth;
    int intr_priority;
    struct {
        uint32_t invert_out : 1;
        uint32_t with_dma : 1;
        uint32_t io_loop_back : 1;
        uint32_t io_od_mode : 1;
        uint32_t init_level : 1;
    } flags;
} rmt_tx_channel_config_t;
typedef struct {
    int loop_count;
    struct {
        uint32_t eot_level : 1;
        uint32_t queue_nonblocking : 1;
    } flags;
} rmt_transmit_config_t;
esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t* config, rmt_channel_handle_t* ret_chan);
esp_err_t rmt_transmit(rmt_channel_handle_t tx_channel, rmt_encoder_handle_t encoder, const void* payload, size_t payload_bytes,
                       const rmt_transmit_config_t* config);
esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t tx_channel, int timeout_ms);
esp_err_t rmt_tx_register_event_callbacks(rmt_channel_handle_t tx_channel, const rmt_tx_event_callbacks_t* cbs, void* user_data);
#ifdef __cplusplus
}
#endif
