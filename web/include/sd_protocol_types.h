#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_bit_defs.h"
#include "freertos/FreeRTOS.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { int csd_ver; int mmc_ver; int capacity; int sector_size; int read_block_len; int card_command_class; int tr_speed; } sdmmc_csd_t;
typedef struct { int mfg_id; int oem_id; char name[8]; int revision; int serial; int date; } sdmmc_cid_t;
typedef struct { uint32_t sd_spec : 4; uint32_t erase_mem_state : 1; uint32_t bus_width : 4; uint32_t reserved : 23; uint32_t rsvd_mnf; } sdmmc_scr_t;
typedef struct { uint32_t alloc_unit_kb : 16; uint32_t erase_size_au : 16; uint32_t cur_bus_width : 2; uint32_t discard_support : 1;
                 uint32_t fule_support : 1; uint32_t erase_timeout : 6; uint32_t erase_offset : 2; uint32_t reserved : 20; } sdmmc_ssr_t;
typedef struct { uint8_t rev; uint8_t power_class; uint8_t erase_mem_state; uint8_t sec_feature; } sdmmc_ext_csd_t;
typedef uint32_t sdmmc_response_t[4];
typedef enum { SDMMC_DELAY_PHASE_0, SDMMC_DELAY_PHASE_1, SDMMC_DELAY_PHASE_2, SDMMC_DELAY_PHASE_3 } sdmmc_delay_phase_t;
typedef struct sdmmc_command_t sdmmc_command_t;
#define SDMMC_HOST_FLAG_1BIT BIT(0)
#define SDMMC_HOST_FLAG_4BIT BIT(1)
#define SDMMC_HOST_FLAG_8BIT BIT(2)
#define SDMMC_HOST_FLAG_SPI BIT(3)
#define SDMMC_HOST_FLAG_DDR BIT(4)
#define SDMMC_HOST_FLAG_DEINIT_ARG BIT(5)
#define SDMMC_FREQ_DEFAULT 20000
#define SDMMC_FREQ_HIGHSPEED 40000
#define SDMMC_FREQ_PROBING 400
#define SDMMC_FREQ_52M 52000
#define SDMMC_FREQ_26M 26000
typedef struct {
    uint32_t flags;
    int slot;
    int max_freq_khz;
    float io_voltage;
    esp_err_t (*init)(void);
    esp_err_t (*set_bus_width)(int slot, size_t width);
    size_t (*get_bus_width)(int slot);
    esp_err_t (*set_bus_ddr_mode)(int slot, bool ddr_enable);
    esp_err_t (*set_card_clk)(int slot, uint32_t freq_khz);
    esp_err_t (*set_cclk_always_on)(int slot, bool cclk_always_on);
    esp_err_t (*do_transaction)(int slot, sdmmc_command_t* cmdinfo);
    union {
        esp_err_t (*deinit)(void);
        esp_err_t (*deinit_p)(int slot);
    };
    esp_err_t (*io_int_enable)(int slot);
    esp_err_t (*io_int_wait)(int slot, TickType_t timeout_ticks);
    int command_timeout_ms;
    esp_err_t (*get_real_freq)(int slot, int* real_freq);
    sdmmc_delay_phase_t input_delay_phase;
    esp_err_t (*set_input_delay)(int slot, sdmmc_delay_phase_t delay_phase);
    void* dma_aligned_buffer;
    void* pwr_ctrl_handle;
    void* get_dma_info;
} sdmmc_host_t;
typedef struct {
    sdmmc_host_t host;
    uint32_t ocr;
    union {
        sdmmc_cid_t cid;
        sdmmc_response_t raw_cid;
    };
    sdmmc_csd_t csd;
    sdmmc_scr_t scr;
    sdmmc_ssr_t ssr;
    sdmmc_ext_csd_t ext_csd;
    uint16_t rca;
    uint16_t max_freq_khz;
    int real_freq_khz;
    uint32_t is_mem : 1;
    uint32_t is_sdio : 1;
    uint32_t is_mmc : 1;
    uint32_t num_io_functions : 3;
    uint32_t log_bus_width : 2;
    uint32_t is_ddr : 1;
    uint32_t reserved : 23;
} sdmmc_card_t;
#ifdef __cplusplus
}
#endif
