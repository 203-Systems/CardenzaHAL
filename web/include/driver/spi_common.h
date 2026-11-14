#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "hal/spi_types.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { SPI_DMA_DISABLED = 0, SPI_DMA_CH1 = 1, SPI_DMA_CH2 = 2, SPI_DMA_CH_AUTO = 3 } spi_common_dma_t;
typedef spi_common_dma_t spi_dma_chan_t;
#define SPICOMMON_BUSFLAG_MASTER (1 << 0)
typedef struct {
    union { int mosi_io_num; int data0_io_num; };
    union { int miso_io_num; int data1_io_num; };
    int sclk_io_num;
    union { int quadwp_io_num; int data2_io_num; };
    union { int quadhd_io_num; int data3_io_num; };
    int data4_io_num;
    int data5_io_num;
    int data6_io_num;
    int data7_io_num;
    bool data_io_default_level;
    int max_transfer_sz;
    uint32_t flags;
    int isr_cpu_id;
    int intr_flags;
} spi_bus_config_t;
// SPI3 carries the display: the HAL initializes it. A second initialization fails as on the device.
esp_err_t spi_bus_initialize(spi_host_device_t host_id, const spi_bus_config_t* bus_config, spi_dma_chan_t dma_chan);
esp_err_t spi_bus_free(spi_host_device_t host_id);
#ifdef __cplusplus
}
#endif
