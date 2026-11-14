#pragma once
#include "esp_err.h"
#include "driver/rmt_types.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t frequency_hz;
    float duty_cycle;
    struct {
        uint32_t polarity_active_low : 1;
        uint32_t always_on : 1;
    } flags;
} rmt_carrier_config_t;
esp_err_t rmt_del_channel(rmt_channel_handle_t channel);
esp_err_t rmt_apply_carrier(rmt_channel_handle_t channel, const rmt_carrier_config_t* config);
esp_err_t rmt_enable(rmt_channel_handle_t channel);
esp_err_t rmt_disable(rmt_channel_handle_t channel);
#ifdef __cplusplus
}
#endif
