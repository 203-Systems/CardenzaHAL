#include "audio.h"
#include "codec/es8156.h"
#include "../board/pins.h"
#include "../system/system.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include <atomic>
#include <mutex>

namespace CardenzaHAL::Audio {

namespace {

i2s_chan_handle_t txChannel = nullptr;
i2s_slot_mode_t outputMode = I2S_SLOT_MODE_MONO;
std::atomic<bool> codecReady{false};
std::mutex codecMutex;
int64_t nextCodecAttemptUs = 0;
bool codecStandbyRequested = false;
bool ownedTxStopPending = false;
constexpr int64_t kCodecRetryUs = 1000 * 1000;
std::atomic<uint8_t> outputVolume{100};

constexpr char kLogTag[] = "CardenzaAudio";

constexpr i2s_port_t kAudioPort = I2S_NUM_1;
constexpr uint32_t kSampleRate = 44100;
// Match the NullPerator baseline: 6 x 256 frames provide ~34.8 ms at 44.1 kHz.
constexpr size_t kDmaFrameCount = 256;
constexpr size_t kDmaDescriptorCount = 6;

i2s_std_slot_config_t MakeSlotConfig(i2s_slot_mode_t mode)
{
    i2s_std_slot_config_t slots = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, mode);
    slots.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    slots.ws_width = 32;
    // ESP32-S3 MONO + BOTH copies one PCM sample to both wire slots.
    // STEREO + BOTH consumes interleaved left/right PCM samples.
    slots.slot_mask = I2S_STD_SLOT_BOTH;
    return slots;
}

esp_err_t EnsureI2sInitialized(i2s_slot_mode_t mode)
{
    if (mode != I2S_SLOT_MODE_MONO && mode != I2S_SLOT_MODE_STEREO)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (txChannel != nullptr)
    {
        if (mode == outputMode)
        {
            return ESP_OK;
        }
        // ESP-IDF rejects reconfiguration while the channel is enabled.
        const i2s_std_slot_config_t slots = MakeSlotConfig(mode);
        const esp_err_t status = i2s_channel_reconfig_std_slot(txChannel, &slots);
        if (status == ESP_OK)
        {
            outputMode = mode;
        }
        return status;
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
        .slot_cfg = MakeSlotConfig(mode),
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

    status = i2s_channel_init_std_mode(txChannel, &standardConfig);
    if (status != ESP_OK)
    {
        i2s_del_channel(txChannel);
        txChannel = nullptr;
    }
    else
    {
        outputMode = mode;
    }

    return status;
}

bool EnsureCodecReady()
{
    if (codecReady.load(std::memory_order_relaxed)) return true;
    const int64_t now = esp_timer_get_time();
    if (now < nextCodecAttemptUs) return false;
    nextCodecAttemptUs = now + kCodecRetryUs;

    if (CardenzaHAL::System::GetI2CBus() == nullptr)
    {
        if (!CardenzaHAL::System::Init())
        {
            ESP_LOGE(kLogTag, "Failed to initialize I2C bus for audio codec");
            return false;
        }
    }

    // Cache the requested level before startup restores its final volume.
    if (!Codec::ES8156::SetVolume(outputVolume.load(std::memory_order_relaxed)) ||
        !Codec::ES8156::Init())
    {
        ESP_LOGW(kLogTag, "ES8156 codec unavailable; a later preparation can retry");
        return false;
    }
    codecReady.store(true, std::memory_order_release);
    return true;
}

}  // namespace

bool Init(i2s_slot_mode_t mode)
{
    std::lock_guard<std::mutex> lock(codecMutex);
    if (codecStandbyRequested || EnsureI2sInitialized(mode) != ESP_OK) return false;
    if (ownedTxStopPending)
    {
        const esp_err_t stopped = i2s_channel_disable(txChannel);
        if (stopped != ESP_OK && stopped != ESP_ERR_INVALID_STATE) return false;
        ownedTxStopPending = false;
    }
    if (codecReady.load(std::memory_order_relaxed)) return true;
    // Initialization owns codec startup, including its clock prerequisite.
    // Fresh DMA is zeroed; preserve a caller's already-running native TX.
    const esp_err_t started = i2s_channel_enable(txChannel);
    if (started != ESP_OK && started != ESP_ERR_INVALID_STATE) return false;
    const bool ready = EnsureCodecReady();
    if (started == ESP_OK)
    {
        ownedTxStopPending = true;
        if (i2s_channel_disable(txChannel) != ESP_OK)
        {
            codecReady.store(false, std::memory_order_release);
            return false;
        }
        ownedTxStopPending = false;
    }
    return ready;
}

i2s_chan_handle_t GetTxChannel()
{
    return txChannel;
}

bool IsCodecReady()
{
    return codecReady.load(std::memory_order_acquire);
}

bool StandbyCodec()
{
    std::lock_guard<std::mutex> lock(codecMutex);
    codecStandbyRequested = true;
    codecReady.store(false, std::memory_order_release);
    nextCodecAttemptUs = 0;
    return Codec::ES8156::SetEnabled(false);
}

bool RestoreOutputPins()
{
    std::lock_guard<std::mutex> lock(codecMutex);
    if (txChannel == nullptr) return false;
    i2s_std_gpio_config_t gpio = {};
    gpio.mclk = I2S_GPIO_UNUSED;
    gpio.bclk = static_cast<gpio_num_t>(Pins::kSpeakerBck);
    gpio.ws = static_cast<gpio_num_t>(Pins::kSpeakerWs);
    gpio.dout = static_cast<gpio_num_t>(Pins::kSpeakerDataOut);
    gpio.din = I2S_GPIO_UNUSED;
    if (i2s_channel_reconfig_std_gpio(txChannel, &gpio) != ESP_OK) return false;
    codecStandbyRequested = false;
    return true;
}

bool SetVolume(uint8_t value)
{
    if (value > 100)
    {
        value = 100;
    }

    std::lock_guard<std::mutex> lock(codecMutex);
    outputVolume.store(value, std::memory_order_relaxed);
    if (!codecReady.load(std::memory_order_relaxed)) return false;
    const bool applied = Codec::ES8156::SetVolume(value);
    if (!applied)
    {
        codecReady.store(false, std::memory_order_release);
        nextCodecAttemptUs = esp_timer_get_time() + kCodecRetryUs;
    }
    return applied;
}

uint8_t GetVolume()
{
    return outputVolume.load(std::memory_order_relaxed);
}

}  // namespace CardenzaHAL::Audio
