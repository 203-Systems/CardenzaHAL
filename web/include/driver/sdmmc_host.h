#pragma once

// The SD card slot (Storage::Init mounts it). On the web target the "card" is the page's /sdcard
// (IndexedDB when the page may keep data, else memory); a page opened without a card has none.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/sdmmc_types.h"
#include "hal/gpio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SDMMC_HOST_SLOT_0 0
#define SDMMC_HOST_SLOT_1 1
#define SDMMC_SLOT_NO_CD GPIO_NUM_NC
#define SDMMC_SLOT_NO_WP GPIO_NUM_NC
#define SDMMC_SLOT_WIDTH_DEFAULT 0
#define SDMMC_SLOT_FLAG_INTERNAL_PULLUP BIT(0)
#define SDMMC_SLOT_FLAG_WP_ACTIVE_HIGH BIT(1)

esp_err_t sdmmc_host_init(void);
esp_err_t sdmmc_host_deinit(void);
esp_err_t sdmmc_host_set_bus_width(int slot, size_t width);
size_t sdmmc_host_get_slot_width(int slot);
esp_err_t sdmmc_host_set_bus_ddr_mode(int slot, bool ddr_enabled);
esp_err_t sdmmc_host_set_card_clk(int slot, uint32_t freq_khz);
esp_err_t sdmmc_host_set_cclk_always_on(int slot, bool cclk_always_on);
esp_err_t sdmmc_host_do_transaction(int slot, sdmmc_command_t* cmdinfo);
esp_err_t sdmmc_host_io_int_enable(int slot);
esp_err_t sdmmc_host_io_int_wait(int slot, TickType_t timeout_ticks);
esp_err_t sdmmc_host_get_real_freq(int slot, int* real_freq_khz);
esp_err_t sdmmc_host_set_input_delay(int slot, sdmmc_delay_phase_t delay_phase);

#define SDMMC_HOST_DEFAULT() { \
    .flags = SDMMC_HOST_FLAG_8BIT | SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_DDR, \
    .slot = SDMMC_HOST_SLOT_1, .max_freq_khz = SDMMC_FREQ_DEFAULT, .io_voltage = 3.3f, \
    .init = &sdmmc_host_init, .set_bus_width = &sdmmc_host_set_bus_width, .get_bus_width = &sdmmc_host_get_slot_width, \
    .set_bus_ddr_mode = &sdmmc_host_set_bus_ddr_mode, .set_card_clk = &sdmmc_host_set_card_clk, \
    .set_cclk_always_on = &sdmmc_host_set_cclk_always_on, .do_transaction = &sdmmc_host_do_transaction, \
    .deinit = &sdmmc_host_deinit, .io_int_enable = sdmmc_host_io_int_enable, .io_int_wait = sdmmc_host_io_int_wait, \
    .command_timeout_ms = 0, .get_real_freq = &sdmmc_host_get_real_freq, .input_delay_phase = SDMMC_DELAY_PHASE_0, \
    .set_input_delay = &sdmmc_host_set_input_delay, .dma_aligned_buffer = NULL, .pwr_ctrl_handle = NULL, .get_dma_info = NULL }

typedef struct {
    union { gpio_num_t gpio_cd; gpio_num_t cd; };
    union { gpio_num_t gpio_wp; gpio_num_t wp; };
    uint8_t width;
    uint32_t flags;
    gpio_num_t clk;
    gpio_num_t cmd;
    gpio_num_t d0;
    gpio_num_t d1;
    gpio_num_t d2;
    gpio_num_t d3;
    gpio_num_t d4;
    gpio_num_t d5;
    gpio_num_t d6;
    gpio_num_t d7;
} sdmmc_slot_config_t;

#define SDMMC_SLOT_CONFIG_DEFAULT() { \
    .cd = SDMMC_SLOT_NO_CD, .wp = SDMMC_SLOT_NO_WP, .width = SDMMC_SLOT_WIDTH_DEFAULT, .flags = 0, \
    .clk = GPIO_NUM_14, .cmd = GPIO_NUM_15, .d0 = GPIO_NUM_2, .d1 = GPIO_NUM_4, .d2 = GPIO_NUM_12, .d3 = GPIO_NUM_13, \
    .d4 = GPIO_NUM_33, .d5 = GPIO_NUM_34, .d6 = GPIO_NUM_35, .d7 = GPIO_NUM_36 }

#ifdef __cplusplus
}
#endif
