#include "ir.h"

#include "../board/pins.h"

#include <driver/gpio.h>
#include <driver/rmt_encoder.h>
#include <driver/rmt_tx.h>
#include <freertos/FreeRTOS.h>

namespace CardenzaHAL::IR {

namespace {

rmt_channel_handle_t irChannel = nullptr;
rmt_encoder_handle_t irEncoder = nullptr;

constexpr uint32_t kResolutionHz = 1000000;
constexpr uint16_t kHeaderMarkUs = 9000;
constexpr uint16_t kHeaderSpaceUs = 4500;
constexpr uint16_t kBitMarkUs = 560;
constexpr uint16_t kOneSpaceUs = 1690;
constexpr uint16_t kZeroSpaceUs = 560;
constexpr uint16_t kTrailerMarkUs = 560;

uint32_t EncodeNecFrame(uint8_t addr, uint8_t cmd)
{
    return static_cast<uint32_t>(addr)
         | (static_cast<uint32_t>(static_cast<uint8_t>(~addr)) << 8)
         | (static_cast<uint32_t>(cmd) << 16)
         | (static_cast<uint32_t>(static_cast<uint8_t>(~cmd)) << 24);
}

rmt_symbol_word_t MakeSymbol(uint16_t duration0, uint16_t duration1)
{
    rmt_symbol_word_t symbol = {};
    symbol.level0 = 1;
    symbol.duration0 = duration0;
    symbol.level1 = 0;
    symbol.duration1 = duration1;
    return symbol;
}

esp_err_t EnsureInitialized()
{
    if (irChannel != nullptr && irEncoder != nullptr)
    {
        return ESP_OK;
    }

    rmt_tx_channel_config_t channelConfig = {};
    channelConfig.gpio_num = static_cast<gpio_num_t>(Pins::kIrTx);
    channelConfig.clk_src = RMT_CLK_SRC_DEFAULT;
    channelConfig.resolution_hz = kResolutionHz;
    channelConfig.mem_block_symbols = 64;
    channelConfig.trans_queue_depth = 4;

    esp_err_t status = rmt_new_tx_channel(&channelConfig, &irChannel);
    if (status != ESP_OK)
    {
        irChannel = nullptr;
        return status;
    }

    rmt_copy_encoder_config_t encoderConfig = {};
    status = rmt_new_copy_encoder(&encoderConfig, &irEncoder);
    if (status != ESP_OK)
    {
        rmt_del_channel(irChannel);
        irChannel = nullptr;
        irEncoder = nullptr;
        return status;
    }

    rmt_carrier_config_t carrier = {};
    carrier.frequency_hz = 38000;
    carrier.duty_cycle = 1.0f / 3.0f;
    status = rmt_apply_carrier(irChannel, &carrier);
    if (status == ESP_OK) status = rmt_enable(irChannel);
    if (status != ESP_OK)
    {
        rmt_del_encoder(irEncoder);
        rmt_del_channel(irChannel);
        irEncoder = nullptr;
        irChannel = nullptr;
    }

    return status;
}

}  // namespace

bool Init()
{
    return EnsureInitialized() == ESP_OK;
}

void Send(uint8_t addr, uint8_t cmd)
{
    if (EnsureInitialized() != ESP_OK)
    {
        return;
    }

    rmt_symbol_word_t symbols[34] = {};
    symbols[0] = MakeSymbol(kHeaderMarkUs, kHeaderSpaceUs);

    const uint32_t frame = EncodeNecFrame(addr, cmd);
    for (int bit = 0; bit < 32; ++bit)
    {
        const bool isOne = ((frame >> bit) & 0x1U) != 0;
        symbols[bit + 1] = MakeSymbol(kBitMarkUs, isOne ? kOneSpaceUs : kZeroSpaceUs);
    }
    symbols[33] = MakeSymbol(kTrailerMarkUs, 0);

    rmt_transmit_config_t transmitConfig = {};
    transmitConfig.loop_count = 0;

    if (rmt_transmit(irChannel, irEncoder, symbols, sizeof(symbols), &transmitConfig) == ESP_OK)
    {
        (void)rmt_tx_wait_all_done(irChannel, portMAX_DELAY);
    }
}

}  // namespace CardenzaHAL::IR
