#pragma once

// UART: the Cardenza has no UART of its own on its connectors (the console is USB). The types are here
// so code that names them builds; using a UART is not supported on the web target.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { UART_NUM_0, UART_NUM_1, UART_NUM_2, UART_NUM_MAX } uart_port_t;
typedef enum { UART_DATA_5_BITS = 0x0, UART_DATA_6_BITS = 0x1, UART_DATA_7_BITS = 0x2, UART_DATA_8_BITS = 0x3, UART_DATA_BITS_MAX = 0x4 } uart_word_length_t;
typedef enum { UART_STOP_BITS_1 = 0x1, UART_STOP_BITS_1_5 = 0x2, UART_STOP_BITS_2 = 0x3, UART_STOP_BITS_MAX = 0x4 } uart_stop_bits_t;
typedef enum { UART_PARITY_DISABLE = 0x0, UART_PARITY_EVEN = 0x2, UART_PARITY_ODD = 0x3 } uart_parity_t;
typedef enum { UART_HW_FLOWCTRL_DISABLE = 0x0, UART_HW_FLOWCTRL_RTS = 0x1, UART_HW_FLOWCTRL_CTS = 0x2, UART_HW_FLOWCTRL_CTS_RTS = 0x3,
               UART_HW_FLOWCTRL_MAX = 0x4 } uart_hw_flowcontrol_t;
typedef int uart_sclk_t;
#define UART_SCLK_DEFAULT 0
#define UART_PIN_NO_CHANGE (-1)
typedef struct {
    int baud_rate;
    uart_word_length_t data_bits;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
    uart_hw_flowcontrol_t flow_ctrl;
    uint8_t rx_flow_ctrl_thresh;
    uart_sclk_t source_clk;
    struct {
        uint32_t allow_pd : 1;
        uint32_t backup_before_sleep : 1;
    } flags;
} uart_config_t;

esp_err_t uart_driver_install(uart_port_t uart_num, int rx_buffer_size, int tx_buffer_size, int queue_size, QueueHandle_t* uart_queue,
                              int intr_alloc_flags) CARDENZA_WEB_UNSUPPORTED;
esp_err_t uart_driver_delete(uart_port_t uart_num) CARDENZA_WEB_UNSUPPORTED;
esp_err_t uart_param_config(uart_port_t uart_num, const uart_config_t* uart_config) CARDENZA_WEB_UNSUPPORTED;
esp_err_t uart_set_pin(uart_port_t uart_num, int tx_io_num, int rx_io_num, int rts_io_num, int cts_io_num) CARDENZA_WEB_UNSUPPORTED;
int uart_write_bytes(uart_port_t uart_num, const void* src, size_t size) CARDENZA_WEB_UNSUPPORTED;
int uart_read_bytes(uart_port_t uart_num, void* buf, uint32_t length, TickType_t ticks_to_wait) CARDENZA_WEB_UNSUPPORTED;
esp_err_t uart_wait_tx_done(uart_port_t uart_num, TickType_t ticks_to_wait) CARDENZA_WEB_UNSUPPORTED;
esp_err_t uart_flush_input(uart_port_t uart_num) CARDENZA_WEB_UNSUPPORTED;
esp_err_t uart_get_buffered_data_len(uart_port_t uart_num, size_t* size) CARDENZA_WEB_UNSUPPORTED;
esp_err_t uart_set_baudrate(uart_port_t uart_num, uint32_t baudrate) CARDENZA_WEB_UNSUPPORTED;

#ifdef __cplusplus
}
#endif
