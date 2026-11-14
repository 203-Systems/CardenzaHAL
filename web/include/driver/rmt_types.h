#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hal/rmt_types.h"
#include "hal/gpio_types.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct rmt_channel_t* rmt_channel_handle_t;
typedef struct rmt_sync_manager_t* rmt_sync_manager_handle_t;
typedef struct rmt_encoder_t* rmt_encoder_handle_t;
typedef struct { size_t num_symbols; } rmt_tx_done_event_data_t;
typedef bool (*rmt_tx_done_callback_t)(rmt_channel_handle_t tx_chan, const rmt_tx_done_event_data_t* edata, void* user_ctx);
#ifdef __cplusplus
}
#endif
