#pragma once
#include <stdint.h>
#include "soc/clk_tree_defs.h"
typedef enum { LEDC_LOW_SPEED_MODE, LEDC_SPEED_MODE_MAX } ledc_mode_t;
typedef enum { LEDC_INTR_DISABLE = 0, LEDC_INTR_FADE_END, LEDC_INTR_MAX } ledc_intr_type_t;
typedef enum { LEDC_DUTY_DIR_DECREASE = 0, LEDC_DUTY_DIR_INCREASE = 1, LEDC_DUTY_DIR_MAX } ledc_duty_direction_t;
typedef soc_periph_ledc_clk_src_legacy_t ledc_clk_cfg_t;
typedef enum { LEDC_APB_CLK = LEDC_USE_APB_CLK, LEDC_SCLK = LEDC_USE_APB_CLK } ledc_clk_src_t;
typedef enum { LEDC_TIMER_0 = 0, LEDC_TIMER_1, LEDC_TIMER_2, LEDC_TIMER_3, LEDC_TIMER_MAX } ledc_timer_t;
typedef enum { LEDC_CHANNEL_0 = 0, LEDC_CHANNEL_1, LEDC_CHANNEL_2, LEDC_CHANNEL_3, LEDC_CHANNEL_4, LEDC_CHANNEL_5, LEDC_CHANNEL_6,
               LEDC_CHANNEL_7, LEDC_CHANNEL_MAX } ledc_channel_t;
typedef enum { LEDC_TIMER_1_BIT = 1, LEDC_TIMER_2_BIT, LEDC_TIMER_3_BIT, LEDC_TIMER_4_BIT, LEDC_TIMER_5_BIT, LEDC_TIMER_6_BIT,
               LEDC_TIMER_7_BIT, LEDC_TIMER_8_BIT, LEDC_TIMER_9_BIT, LEDC_TIMER_10_BIT, LEDC_TIMER_11_BIT, LEDC_TIMER_12_BIT,
               LEDC_TIMER_13_BIT, LEDC_TIMER_14_BIT, LEDC_TIMER_BIT_MAX } ledc_timer_bit_t;
typedef enum { LEDC_FADE_NO_WAIT = 0, LEDC_FADE_WAIT_DONE, LEDC_FADE_MAX } ledc_fade_mode_t;
typedef enum { LEDC_SLEEP_MODE_NO_ALIVE_NO_PD = 0, LEDC_SLEEP_MODE_NO_ALIVE_ALLOW_PD, LEDC_SLEEP_MODE_KEEP_ALIVE, LEDC_SLEEP_MODE_INVALID } ledc_sleep_mode_t;
