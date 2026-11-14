#pragma once
#include <stdint.h>
#include "soc/clk_tree_defs.h"
typedef enum { I2C_NUM_0 = 0, I2C_NUM_1, I2C_NUM_MAX } i2c_port_t;
typedef enum { I2C_ADDR_BIT_LEN_7 = 0, I2C_ADDR_BIT_LEN_10 = 1 } i2c_addr_bit_len_t;
typedef enum { I2C_MODE_SLAVE = 0, I2C_MODE_MASTER, I2C_MODE_MAX } i2c_mode_t;
typedef enum { I2C_MASTER_WRITE = 0, I2C_MASTER_READ } i2c_rw_t;
typedef enum { I2C_ADDR_BIT_7 = 0, I2C_ADDR_BIT_10 } i2c_addr_mode_t;
typedef soc_periph_i2c_clk_src_t i2c_clock_source_t;
