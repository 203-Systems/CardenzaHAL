#pragma once
#include "esp_err.h"
#include "driver/rmt_types.h"
#ifdef __cplusplus
extern "C" {
#endif
// Only the copy encoder exists on the web target: the symbols are what the IR LED would send.
typedef struct {
} rmt_copy_encoder_config_t;
esp_err_t rmt_new_copy_encoder(const rmt_copy_encoder_config_t* config, rmt_encoder_handle_t* ret_encoder);
esp_err_t rmt_del_encoder(rmt_encoder_handle_t encoder);
esp_err_t rmt_encoder_reset(rmt_encoder_handle_t encoder);
#ifdef __cplusplus
}
#endif
