#include "mic.h"
#include "../board/pins.h"

#include <cstddef>
#include <cstdint>

#include <driver/gpio.h>
#include <driver/i2s_pdm.h>
#include <driver/i2s_std.h>

namespace CardenzaHAL::Mic {

namespace {

i2s_chan_handle_t micChannel = nullptr;

constexpr i2s_port_t kMicPort = I2S_NUM_0;
constexpr uint32_t kSampleRate = 44100;
constexpr size_t kDmaFrameCount = 256;
constexpr size_t kDmaDescriptorCount = 8;

esp_err_t EnsureInitialized()
{
    if (micChannel != nullptr)
    {
        return ESP_OK;
    }

    i2s_chan_config_t channelConfig = I2S_CHANNEL_DEFAULT_CONFIG(kMicPort, I2S_ROLE_MASTER);
    channelConfig.dma_desc_num = kDmaDescriptorCount;
    channelConfig.dma_frame_num = kDmaFrameCount;
    channelConfig.auto_clear = false;

    esp_err_t status = i2s_new_channel(&channelConfig, nullptr, &micChannel);
    if (status != ESP_OK)
    {
        micChannel = nullptr;
        return status;
    }

    i2s_pdm_rx_config_t pdmConfig = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(kSampleRate),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = static_cast<gpio_num_t>(Pins::kMicWs),
            .din = static_cast<gpio_num_t>(Pins::kMicDataIn),
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };

    status = i2s_channel_init_pdm_rx_mode(micChannel, &pdmConfig);
    if (status != ESP_OK)
    {
        i2s_del_channel(micChannel);
        micChannel = nullptr;
    }

    return status;
}

}  // namespace

bool Init()
{
    return EnsureInitialized() == ESP_OK;
}

i2s_chan_handle_t InputChannel()
{
    if (EnsureInitialized() != ESP_OK)
    {
        return nullptr;
    }

    return micChannel;
}

bool RestoreInputPins()
{
    if (micChannel == nullptr) return false;
    i2s_pdm_rx_gpio_config_t pins = {};
    pins.clk = static_cast<gpio_num_t>(Pins::kMicWs);
    pins.din = static_cast<gpio_num_t>(Pins::kMicDataIn);
    return i2s_channel_reconfig_pdm_rx_gpio(micChannel, &pins) == ESP_OK;
}

}  // namespace CardenzaHAL::Mic
