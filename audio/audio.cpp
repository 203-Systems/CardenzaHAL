#include "audio.h"
#include "codec/es8156.h"
#include "../board/pins.h"
#include "../system/system.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>

#include <atomic>

namespace CardenzaHAL::Audio {

namespace {

enum class CodecStatus : uint8_t {
    NotProbed,
    Available,
    Unavailable,
};

i2s_chan_handle_t txChannel = nullptr;
CodecStatus codecStatus = CodecStatus::NotProbed;
std::atomic<uint8_t> outputVolume{100};

constexpr char kLogTag[] = "CardenzaAudio";

constexpr i2s_port_t kAudioPort = I2S_NUM_1;
constexpr uint32_t kSampleRate = 44100;
// 3 x 128 frames (~8.7 ms at 44.1 kHz) keeps output latency low; the old 8 x 256 queued ~46 ms.
constexpr size_t kDmaFrameCount = 128;
constexpr size_t kDmaDescriptorCount = 3;

esp_err_t EnsureI2sInitialized()
{
    if (txChannel != nullptr)
    {
        return ESP_OK;
    }

    i2s_chan_config_t channelConfig = I2S_CHANNEL_DEFAULT_CONFIG(kAudioPort, I2S_ROLE_MASTER);
    channelConfig.dma_desc_num = kDmaDescriptorCount;
    channelConfig.dma_frame_num = kDmaFrameCount;
    channelConfig.auto_clear = true;

    esp_err_t status = i2s_new_channel(&channelConfig, &txChannel, nullptr);
    if (status != ESP_OK)
    {
        txChannel = nullptr;
        return status;
    }

    i2s_std_config_t standardConfig = {
        .clk_cfg = {
            .sample_rate_hz = kSampleRate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = static_cast<gpio_num_t>(Pins::kSpeakerBck),
            .ws = static_cast<gpio_num_t>(Pins::kSpeakerWs),
            .dout = static_cast<gpio_num_t>(Pins::kSpeakerDataOut),
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    standardConfig.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    // Philips WS must span the full slot, including the 16 padding bits.
    standardConfig.slot_cfg.ws_width = 32;
    standardConfig.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    status = i2s_channel_init_std_mode(txChannel, &standardConfig);
    if (status != ESP_OK)
    {
        i2s_del_channel(txChannel);
        txChannel = nullptr;
    }

    return status;
}

bool EnsureCodecReady()
{
    if (codecStatus == CodecStatus::Available)
    {
        return true;
    }

    if (codecStatus == CodecStatus::Unavailable)
    {
        return false;
    }

    if (CardenzaHAL::System::GetI2CBus() == nullptr)
    {
        if (!CardenzaHAL::System::Init())
        {
            ESP_LOGE(kLogTag, "Failed to initialize I2C bus for audio codec");
            codecStatus = CodecStatus::Unavailable;
            return false;
        }
    }

    if (!Codec::ES8156::Init())
    {
        ESP_LOGW(kLogTag, "ES8156 codec unavailable, continuing with I2S only");
        codecStatus = CodecStatus::Unavailable;
        return false;
    }

    codecStatus = CodecStatus::Available;
    return true;
}

bool ApplyCachedVolume()
{
    if (!EnsureCodecReady())
    {
        return true;
    }

    return Codec::ES8156::SetVolume(outputVolume.load(std::memory_order_relaxed));
}

}  // namespace

bool Init()
{
    return EnsureI2sInitialized() == ESP_OK;
}

i2s_chan_handle_t GetTxChannel()
{
    if (EnsureI2sInitialized() != ESP_OK)
    {
        return nullptr;
    }

    return txChannel;
}

bool SetVolume(uint8_t value)
{
    if (value > 100)
    {
        value = 100;
    }

    outputVolume.store(value, std::memory_order_relaxed);
    return ApplyCachedVolume();
}

uint8_t GetVolume()
{
    return outputVolume.load(std::memory_order_relaxed);
}

}  // namespace CardenzaHAL::Audio
