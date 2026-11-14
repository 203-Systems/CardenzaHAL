#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "hal/i2c_types.h"
#include "soc/soc_caps.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef int i2c_port_num_t;
typedef enum { I2C_STATUS_READ, I2C_STATUS_WRITE, I2C_STATUS_START, I2C_STATUS_STOP, I2C_STATUS_IDLE, I2C_STATUS_ACK_ERROR,
               I2C_STATUS_DONE, I2C_STATUS_TIMEOUT } i2c_master_status_t;
typedef enum { I2C_EVENT_ALIVE, I2C_EVENT_DONE, I2C_EVENT_NACK, I2C_EVENT_TIMEOUT } i2c_master_event_t;
typedef enum { I2C_MASTER_CMD_START, I2C_MASTER_CMD_WRITE, I2C_MASTER_CMD_READ, I2C_MASTER_CMD_STOP } i2c_master_command_t;
typedef enum { I2C_ACK_VAL = 0, I2C_NACK_VAL = 1 } __attribute__((packed)) i2c_ack_value_t;
typedef struct i2c_master_bus_t* i2c_master_bus_handle_t;
typedef struct i2c_master_dev_t* i2c_master_dev_handle_t;
typedef struct i2c_slave_dev_t* i2c_slave_dev_handle_t;
typedef struct {
    i2c_master_event_t event;
} i2c_master_event_data_t;
typedef bool (*i2c_master_callback_t)(i2c_master_dev_handle_t i2c_dev, const i2c_master_event_data_t* evt_data, void* arg);
#ifdef __cplusplus
}
#endif
