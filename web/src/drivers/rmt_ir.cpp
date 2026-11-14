// RMT transmit on the web target, for the IR LED: a transmission takes the time its symbols last,
// and an NEC frame among them is decoded for the page (it shows what the LED sent).

#include <cstdlib>
#include <cstring>

#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"

#include "../kernel/kernel.h"
#include "../platform.h"

namespace k = cardenza_web::kernel;

struct rmt_channel_t {
    uint32_t resolution;
    bool enabled;
    uint64_t busyUntil;
    rmt_tx_done_callback_t done;
    void* context;
};

struct rmt_encoder_t {
    int kind;
};

namespace {

bool Near(uint32_t value, uint32_t expected)
{
    return value + expected / 4 >= expected && value <= expected + expected / 4;
}

// NEC: a 9 ms mark, a 4.5 ms space, 32 bits (560 us mark, 560 / 1690 us space), LSB first.
void Decode(const rmt_symbol_word_t* symbols, size_t count, uint32_t resolution)
{
    const auto micros = [resolution](uint32_t ticks) { return static_cast<uint32_t>(static_cast<uint64_t>(ticks) * 1000000u / resolution); };
    for (size_t start = 0; start + 33 <= count; ++start)
    {
        if (!Near(micros(symbols[start].duration0), 9000) || !Near(micros(symbols[start].duration1), 4500)) continue;
        uint32_t frame = 0;
        bool ok = true;
        for (int bit = 0; bit < 32 && ok; ++bit)
        {
            const uint32_t space = micros(symbols[start + 1 + bit].duration1);
            if (Near(space, 1690)) frame |= 1u << bit;
            else if (!Near(space, 560)) ok = false;
        }
        if (!ok) continue;
        const uint8_t address = static_cast<uint8_t>(frame);
        const uint8_t command = static_cast<uint8_t>(frame >> 16);
        const bool valid = static_cast<uint8_t>(~(frame >> 8)) == address && static_cast<uint8_t>(~(frame >> 24)) == command;
        cardenza_web::platform::IrSent(address, command, valid);
        return;
    }
}

}  // namespace

extern "C" {

esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t* config, rmt_channel_handle_t* ret_chan)
{
    if (config == nullptr || ret_chan == nullptr || config->resolution_hz == 0) return ESP_ERR_INVALID_ARG;
    auto* channel = static_cast<rmt_channel_t*>(std::calloc(1, sizeof(rmt_channel_t)));
    if (channel == nullptr) return ESP_ERR_NO_MEM;
    channel->resolution = config->resolution_hz;
    *ret_chan = channel;
    return ESP_OK;
}

esp_err_t rmt_del_channel(rmt_channel_handle_t channel)
{
    if (channel == nullptr) return ESP_ERR_INVALID_ARG;
    std::free(channel);
    return ESP_OK;
}

esp_err_t rmt_apply_carrier(rmt_channel_handle_t channel, const rmt_carrier_config_t*) { return channel == nullptr ? ESP_ERR_INVALID_ARG : ESP_OK; }

esp_err_t rmt_enable(rmt_channel_handle_t channel)
{
    if (channel == nullptr) return ESP_ERR_INVALID_ARG;
    channel->enabled = true;
    return ESP_OK;
}

esp_err_t rmt_disable(rmt_channel_handle_t channel)
{
    if (channel == nullptr) return ESP_ERR_INVALID_ARG;
    channel->enabled = false;
    return ESP_OK;
}

esp_err_t rmt_new_copy_encoder(const rmt_copy_encoder_config_t*, rmt_encoder_handle_t* ret_encoder)
{
    if (ret_encoder == nullptr) return ESP_ERR_INVALID_ARG;
    auto* encoder = static_cast<rmt_encoder_t*>(std::calloc(1, sizeof(rmt_encoder_t)));
    if (encoder == nullptr) return ESP_ERR_NO_MEM;
    *ret_encoder = encoder;
    return ESP_OK;
}

esp_err_t rmt_del_encoder(rmt_encoder_handle_t encoder)
{
    std::free(encoder);
    return ESP_OK;
}

esp_err_t rmt_encoder_reset(rmt_encoder_handle_t encoder) { return encoder == nullptr ? ESP_ERR_INVALID_ARG : ESP_OK; }

esp_err_t rmt_transmit(rmt_channel_handle_t channel, rmt_encoder_handle_t encoder, const void* payload, size_t bytes, const rmt_transmit_config_t*)
{
    if (channel == nullptr || encoder == nullptr || payload == nullptr) return ESP_ERR_INVALID_ARG;
    if (!channel->enabled) return ESP_ERR_INVALID_STATE;
    const auto* symbols = static_cast<const rmt_symbol_word_t*>(payload);
    const size_t count = bytes / sizeof(rmt_symbol_word_t);
    uint64_t ticks = 0;
    for (size_t index = 0; index < count; ++index) ticks += symbols[index].duration0 + symbols[index].duration1;
    const uint64_t now = k::NowUs();
    const uint64_t start = channel->busyUntil > now ? channel->busyUntil : now;
    channel->busyUntil = start + ticks * 1000000u / channel->resolution;
    Decode(symbols, count, channel->resolution);
    return ESP_OK;
}

esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t channel, int timeout_ms)
{
    if (channel == nullptr) return ESP_ERR_INVALID_ARG;
    const uint64_t deadline = timeout_ms < 0 ? k::kForever : k::NowUs() + static_cast<uint64_t>(timeout_ms) * 1000;
    if (channel->busyUntil > deadline) return ESP_ERR_TIMEOUT;
    k::SleepUntil(channel->busyUntil);
    return ESP_OK;
}

esp_err_t rmt_tx_register_event_callbacks(rmt_channel_handle_t channel, const rmt_tx_event_callbacks_t* cbs, void* user_data)
{
    if (channel == nullptr || cbs == nullptr) return ESP_ERR_INVALID_ARG;
    channel->done = cbs->on_trans_done;
    channel->context = user_data;
    return ESP_OK;
}

}  // extern "C"
