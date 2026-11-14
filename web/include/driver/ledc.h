#pragma once

// LEDC PWM on the web target keeps each channel's duty. A channel on GPIO38 drives the display
// backlight (what the HAL does with timer 0 / channel 7). Fades jump to their target.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "hal/ledc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LEDC_APB_CLK_HZ (80 * 1000000)
#define LEDC_ERR_DUTY (0xFFFFFFFF)
#define LEDC_ERR_VAL (-1)

typedef intr_handle_t ledc_isr_handle_t;
typedef enum { LEDC_FADE_END_EVT } ledc_cb_event_t;
typedef struct {
    ledc_cb_event_t event;
    uint32_t speed_mode;
    uint32_t channel;
    uint32_t duty;
} ledc_cb_param_t;
typedef bool (*ledc_cb_t)(const ledc_cb_param_t* param, void* user_arg);
typedef struct {
    ledc_cb_t fade_cb;
} ledc_cbs_t;

typedef struct {
    int gpio_num;
    ledc_mode_t speed_mode;
    ledc_channel_t channel;
    ledc_intr_type_t intr_type;
    ledc_timer_t timer_sel;
    uint32_t duty;
    int hpoint;
    ledc_sleep_mode_t sleep_mode;
    struct {
        unsigned int output_invert : 1;
    } flags;
} ledc_channel_config_t;

typedef struct {
    ledc_mode_t speed_mode;
    ledc_timer_bit_t duty_resolution;
    ledc_timer_t timer_num;
    uint32_t freq_hz;
    ledc_clk_cfg_t clk_cfg;
    bool deconfigure;
} ledc_timer_config_t;

esp_err_t ledc_channel_config(const ledc_channel_config_t* ledc_conf);
uint32_t ledc_find_suitable_duty_resolution(uint32_t src_clk_freq, uint32_t timer_freq);
esp_err_t ledc_timer_config(const ledc_timer_config_t* timer_conf);
esp_err_t ledc_update_duty(ledc_mode_t speed_mode, ledc_channel_t channel);
esp_err_t ledc_set_pin(int gpio_num, ledc_mode_t speed_mode, ledc_channel_t ledc_channel);
esp_err_t ledc_stop(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t idle_level);
esp_err_t ledc_set_freq(ledc_mode_t speed_mode, ledc_timer_t timer_num, uint32_t freq_hz);
uint32_t ledc_get_freq(ledc_mode_t speed_mode, ledc_timer_t timer_num);
esp_err_t ledc_set_duty_with_hpoint(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t duty, uint32_t hpoint);
int ledc_get_hpoint(ledc_mode_t speed_mode, ledc_channel_t channel);
esp_err_t ledc_set_duty(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t duty);
uint32_t ledc_get_duty(ledc_mode_t speed_mode, ledc_channel_t channel);
esp_err_t ledc_timer_rst(ledc_mode_t speed_mode, ledc_timer_t timer_sel);
esp_err_t ledc_timer_pause(ledc_mode_t speed_mode, ledc_timer_t timer_sel);
esp_err_t ledc_timer_resume(ledc_mode_t speed_mode, ledc_timer_t timer_sel);
esp_err_t ledc_bind_channel_timer(ledc_mode_t speed_mode, ledc_channel_t channel, ledc_timer_t timer_sel);
esp_err_t ledc_set_fade_with_time(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t target_duty, int desired_fade_time_ms);
esp_err_t ledc_set_fade_with_step(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t target_duty, uint32_t scale, uint32_t cycle_num);
esp_err_t ledc_fade_func_install(int intr_alloc_flags);
void ledc_fade_func_uninstall(void);
esp_err_t ledc_fade_start(ledc_mode_t speed_mode, ledc_channel_t channel, ledc_fade_mode_t fade_mode);
esp_err_t ledc_fade_stop(ledc_mode_t speed_mode, ledc_channel_t channel);
esp_err_t ledc_set_duty_and_update(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t duty, uint32_t hpoint);
esp_err_t ledc_set_fade_time_and_start(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t target_duty, uint32_t desired_fade_time_ms,
                                       ledc_fade_mode_t fade_mode);
esp_err_t ledc_cb_register(ledc_mode_t speed_mode, ledc_channel_t channel, ledc_cbs_t* cbs, void* user_arg);
esp_err_t ledc_isr_register(void (*fn)(void*), void* arg, int intr_alloc_flags, ledc_isr_handle_t* handle) CARDENZA_WEB_UNSUPPORTED;

#ifdef __cplusplus
}
#endif
