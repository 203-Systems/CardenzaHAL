// I2S on the web target.
//
// TX: a channel's DMA is a queue of dma_desc_num buffers of dma_frame_num frames, read at the sample
// rate by the page's audio clock (audio::ClockFrames). i2s_channel_write copies into the queue and
// blocks while it is full (it wakes as each buffer is sent, like the device); what is written plays
// at the position the DMA will reach it, on the speaker when the channel drives the codec (BCLK 41,
// WS 43, DOUT 42) and GPIO43 is the speaker's. When the queue runs dry the DMA sends silence and
// on_send_q_ovf fires; on_sent fires for every buffer sent.
//
// RX: a channel fills at the sample rate from the page's microphone (silence without one, or when
// GPIO43 belongs to the speaker); i2s_channel_read blocks until it has what it asked for; an unread
// queue overflows (on_recv_q_ovf), on_recv fires for every buffer received.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "driver/i2s_common.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"

#include "audio_state.h"
#include "../esp/heap_internal.h"
#include "../kernel/kernel.h"
#include "../kernel/raw_alloc.h"

namespace k = cardenza_web::kernel;
namespace a = cardenza_web::audio;

struct i2s_channel_obj_t {
    k::TimedSource source;  // first
    i2s_port_t port;
    bool tx;
    i2s_role_t role;
    i2s_comm_mode_t mode;
    uint32_t descriptors;
    uint32_t framesPerDescriptor;
    bool autoClear;
    uint32_t rate;
    uint32_t bits;
    uint32_t channels;
    int bclk;
    int ws;
    int dout;
    int din;
    bool enabled;
    // TX: positions in frames of this channel's rate on the audio clock
    int64_t readPosition;
    int64_t queueEnd;
    int64_t nextBoundary;
    bool dry;
    // RX
    int64_t rxRead;
    std::vector<uint8_t>* preload;
    i2s_event_callbacks_t callbacks;
    void* context;
    uint8_t* scratch;  // one DMA buffer, for the callbacks' event data
    void* dmaCharge;   // the DMA buffers the device takes from internal RAM (heap_internal.h)
    uint32_t slotBits;
    k::WaitQueue waiters;
    i2s_channel_obj_t* pair;
};

namespace {

constexpr int kPorts = 2;
constexpr int kSpeakerBclk = 41;
constexpr int kSpeakerWs = 43;
constexpr int kSpeakerData = 42;
constexpr int kMicClock = 43;

i2s_channel_obj_t* g_tx[kPorts] = {};
i2s_channel_obj_t* g_rx[kPorts] = {};

uint32_t BytesPerSample(const i2s_channel_obj_t* channel)
{
    return channel->bits <= 8 ? 1 : (channel->bits <= 16 ? 2 : (channel->bits <= 24 ? 3 : 4));
}

uint32_t BytesPerFrame(const i2s_channel_obj_t* channel) { return BytesPerSample(channel) * channel->channels; }
uint32_t Capacity(const i2s_channel_obj_t* channel) { return channel->descriptors * channel->framesPerDescriptor; }

int64_t Now(const i2s_channel_obj_t* channel)
{
    return static_cast<int64_t>(a::ClockFrames() * channel->rate / a::kRate);
}

uint64_t DueUs(const i2s_channel_obj_t* channel, int64_t position)
{
    const int64_t now = Now(channel);
    if (position <= now) return k::NowUs();
    return k::NowUs() + static_cast<uint64_t>((position - now) * 1000000 / channel->rate) + 1;
}

bool DrivesSpeaker(const i2s_channel_obj_t* channel)
{
    return channel->bclk == kSpeakerBclk && channel->ws == kSpeakerWs && channel->dout == kSpeakerData &&
           a::Pin43Owner() == a::Pin43::Speaker;
}

bool HearsMicrophone(const i2s_channel_obj_t* channel)
{
    return channel->mode == I2S_COMM_MODE_PDM && channel->ws == kMicClock && a::Pin43Owner() == a::Pin43::Microphone;
}

float SampleAt(const uint8_t* data, uint32_t bytes)
{
    switch (bytes)
    {
        case 1: return static_cast<float>(static_cast<int8_t>(data[0])) / 128.0f;
        case 2:
        {
            int16_t value;
            std::memcpy(&value, data, 2);
            return static_cast<float>(value) / 32768.0f;
        }
        case 3:
        {
            const int32_t value = static_cast<int32_t>((static_cast<uint32_t>(data[2]) << 24) | (data[1] << 16) | (data[0] << 8)) >> 8;
            return static_cast<float>(value) / 8388608.0f;
        }
        default:
        {
            int32_t value;
            std::memcpy(&value, data, 4);
            return static_cast<float>(value) / 2147483648.0f;
        }
    }
}

// Frames of the channel's format to the speaker at a channel position.
void ToSpeaker(const i2s_channel_obj_t* channel, int64_t position, const uint8_t* data, uint32_t frames)
{
    if (!DrivesSpeaker(channel)) return;
    const float gain = a::CodecGain();
    const uint32_t sampleBytes = BytesPerSample(channel);
    const uint32_t frameBytes = BytesPerFrame(channel);
    float mono[512];
    uint32_t done = 0;
    while (done < frames)
    {
        const uint32_t count = std::min<uint32_t>(frames - done, 512);
        for (uint32_t index = 0; index < count; ++index)
        {
            const uint8_t* frame = data + static_cast<size_t>(done + index) * frameBytes;
            float value = SampleAt(frame, sampleBytes);
            // The board has one speaker: a stereo stream is heard as its mix.
            if (channel->channels == 2) value = 0.5f * (value + SampleAt(frame + sampleBytes, sampleBytes));
            mono[index] = value * gain;
        }
        if (channel->rate == a::kRate)
        {
            a::Speaker(position + done, mono, count);
        }
        else
        {
            // Another rate: nearest frames on the speaker's 44.1 kHz grid.
            const int64_t start = (position + done) * static_cast<int64_t>(a::kRate) / channel->rate;
            const int64_t end = (position + done + count) * static_cast<int64_t>(a::kRate) / channel->rate;
            float resampled[2048];
            const int64_t out = std::min<int64_t>(end - start, 2048);
            for (int64_t index = 0; index < out; ++index)
            {
                const int64_t source = (start + index) * channel->rate / a::kRate - (position + done);
                resampled[index] = mono[std::clamp<int64_t>(source, 0, count - 1)];
            }
            a::Speaker(start, resampled, static_cast<std::size_t>(out));
        }
        done += count;
    }
}

void Notify(i2s_channel_obj_t* channel, i2s_isr_callback_t callback)
{
    if (callback == nullptr) return;
    i2s_event_data_t event{};
    event.dma_buf = channel->scratch;
    event.size = channel->framesPerDescriptor * BytesPerFrame(channel);
    callback(channel, &event, channel->context);
}

// TX: the DMA's progress up to now.
void AdvanceTx(i2s_channel_obj_t* channel)
{
    if (!channel->enabled) return;
    const int64_t now = Now(channel);
    if (now <= channel->readPosition) return;
    channel->readPosition = now;
    int fired = 0;
    while (channel->nextBoundary <= now)
    {
        channel->nextBoundary += channel->framesPerDescriptor;
        if (fired++ < 8) Notify(channel, channel->callbacks.on_sent);
        if (channel->queueEnd < channel->nextBoundary - static_cast<int64_t>(channel->framesPerDescriptor) && fired < 8)
            Notify(channel, channel->callbacks.on_send_q_ovf);
    }
    if (channel->queueEnd < now)
    {
        channel->dry = true;
        channel->queueEnd = now;  // what is written next plays from here: the gap was silence
    }
}

// RX: frames waiting to be read; an overflow drops the oldest.
int64_t AvailableRx(i2s_channel_obj_t* channel)
{
    const int64_t now = Now(channel);
    if (now - channel->rxRead > static_cast<int64_t>(Capacity(channel)))
    {
        channel->rxRead = now - Capacity(channel);
        Notify(channel, channel->callbacks.on_recv_q_ovf);
    }
    while (channel->nextBoundary <= now)
    {
        channel->nextBoundary += channel->framesPerDescriptor;
        Notify(channel, channel->callbacks.on_recv);
    }
    return now - channel->rxRead;
}

void Rearm(i2s_channel_obj_t* channel)
{
    k::Reschedule(&channel->source, channel->enabled ? DueUs(channel, channel->nextBoundary) : k::kForever);
}

void Fire(k::TimedSource* source, uint64_t)
{
    auto* channel = reinterpret_cast<i2s_channel_obj_t*>(source);
    if (!channel->enabled) return;
    if (channel->tx) AdvanceTx(channel);
    else AvailableRx(channel);
    k::WakeAll(&channel->waiters);
    Rearm(channel);
}

i2s_channel_obj_t* NewChannel(const i2s_chan_config_t* config, i2s_port_t port, bool tx)
{
    void* memory = std::calloc(1, sizeof(i2s_channel_obj_t));
    if (memory == nullptr) return nullptr;
    auto* channel = new (memory) i2s_channel_obj_t{};
    channel->source.fire = Fire;
    channel->port = port;
    channel->tx = tx;
    channel->role = config->role;
    channel->mode = I2S_COMM_MODE_NONE;
    channel->descriptors = config->dma_desc_num == 0 ? 6 : config->dma_desc_num;
    channel->framesPerDescriptor = config->dma_frame_num == 0 ? 240 : config->dma_frame_num;
    channel->autoClear = config->auto_clear_after_cb || config->auto_clear_before_cb;
    channel->rate = a::kRate;
    channel->bits = 16;
    channel->channels = 2;
    channel->bclk = channel->ws = channel->dout = channel->din = -1;
    k::Register(&channel->source);
    return channel;
}

bool Valid(const i2s_channel_obj_t* channel) { return channel != nullptr; }

// The DMA buffers: dma_desc_num of dma_frame_num frames as they go on the wire (both slots of a
// standard channel, at the slot width), as ESP-IDF allocates them.
void ChargeDma(i2s_channel_obj_t* channel)
{
    cardenza_web::heap::Refund(channel->dmaCharge);
    const uint32_t slotBytes = (channel->slotBits != 0 ? channel->slotBits : channel->bits) / 8;
    const uint32_t wireSlots = channel->mode == I2S_COMM_MODE_STD ? 2 : channel->channels;
    channel->dmaCharge = cardenza_web::heap::Charge(static_cast<std::size_t>(Capacity(channel)) * slotBytes * wireSlots);
}

void ApplyStdSlot(i2s_channel_obj_t* channel, const i2s_std_slot_config_t* slot)
{
    channel->bits = slot->data_bit_width;
    channel->slotBits = slot->slot_bit_width;
    // MONO takes one sample a frame (ESP32-S3 copies it to both wire slots); STEREO two.
    channel->channels = slot->slot_mode == I2S_SLOT_MODE_MONO ? 1 : 2;
}

void ApplyStdGpio(i2s_channel_obj_t* channel, const i2s_std_gpio_config_t* gpio)
{
    channel->bclk = gpio->bclk;
    channel->ws = gpio->ws;
    channel->dout = gpio->dout;
    channel->din = gpio->din;
    if (channel->ws == kSpeakerWs || channel->bclk == kSpeakerWs) a::ClaimPin43(a::Pin43::Speaker);
}

void ApplyPdmGpio(i2s_channel_obj_t* channel, const i2s_pdm_rx_gpio_config_t* gpio)
{
    channel->ws = gpio->clk;
    channel->din = gpio->din;
    if (gpio->clk == kMicClock) a::ClaimPin43(a::Pin43::Microphone);
}

}  // namespace

extern "C" {

esp_err_t i2s_new_channel(const i2s_chan_config_t* chan_cfg, i2s_chan_handle_t* ret_tx_handle, i2s_chan_handle_t* ret_rx_handle)
{
    if (chan_cfg == nullptr || (ret_tx_handle == nullptr && ret_rx_handle == nullptr)) return ESP_ERR_INVALID_ARG;
    int port = chan_cfg->id;
    if (port == I2S_NUM_AUTO)
    {
        port = -1;
        for (int candidate = 0; candidate < kPorts && port < 0; ++candidate)
            if ((ret_tx_handle == nullptr || g_tx[candidate] == nullptr) && (ret_rx_handle == nullptr || g_rx[candidate] == nullptr)) port = candidate;
        if (port < 0) return ESP_ERR_NOT_FOUND;
    }
    if (port < 0 || port >= kPorts) return ESP_ERR_INVALID_ARG;
    if ((ret_tx_handle != nullptr && g_tx[port] != nullptr) || (ret_rx_handle != nullptr && g_rx[port] != nullptr)) return ESP_ERR_NOT_FOUND;
    i2s_channel_obj_t* tx = nullptr;
    i2s_channel_obj_t* rx = nullptr;
    if (ret_tx_handle != nullptr && (tx = NewChannel(chan_cfg, static_cast<i2s_port_t>(port), true)) == nullptr) return ESP_ERR_NO_MEM;
    if (ret_rx_handle != nullptr && (rx = NewChannel(chan_cfg, static_cast<i2s_port_t>(port), false)) == nullptr) return ESP_ERR_NO_MEM;
    if (tx != nullptr && rx != nullptr)
    {
        tx->pair = rx;
        rx->pair = tx;
    }
    for (i2s_channel_obj_t* channel : {tx, rx})
    {
        if (channel == nullptr) continue;
        channel->scratch = static_cast<uint8_t*>(cardenza_web::RawAlloc(channel->framesPerDescriptor * 8));
        if (channel->scratch != nullptr) std::memset(channel->scratch, 0, channel->framesPerDescriptor * 8);
    }
    if (tx != nullptr)
    {
        g_tx[port] = tx;
        *ret_tx_handle = tx;
    }
    if (rx != nullptr)
    {
        g_rx[port] = rx;
        *ret_rx_handle = rx;
    }
    return ESP_OK;
}

esp_err_t i2s_del_channel(i2s_chan_handle_t handle)
{
    if (!Valid(handle)) return ESP_ERR_INVALID_ARG;
    if (handle->enabled) return ESP_ERR_INVALID_STATE;
    k::Unregister(&handle->source);
    (handle->tx ? g_tx : g_rx)[handle->port] = nullptr;
    if (handle->pair != nullptr) handle->pair->pair = nullptr;
    delete handle->preload;
    cardenza_web::heap::Refund(handle->dmaCharge);
    cardenza_web::RawFree(handle->scratch);
    handle->~i2s_channel_obj_t();
    std::free(handle);
    return ESP_OK;
}

esp_err_t i2s_channel_get_info(i2s_chan_handle_t handle, i2s_chan_info_t* info)
{
    if (!Valid(handle) || info == nullptr) return ESP_ERR_INVALID_ARG;
    info->id = handle->port;
    info->role = handle->role;
    info->dir = handle->tx ? I2S_DIR_TX : I2S_DIR_RX;
    info->mode = handle->mode;
    info->pair_chan = handle->pair;
    info->total_dma_buf_size = Capacity(handle) * BytesPerFrame(handle);
    return ESP_OK;
}

esp_err_t i2s_channel_enable(i2s_chan_handle_t handle)
{
    if (!Valid(handle)) return ESP_ERR_INVALID_ARG;
    if (handle->mode == I2S_COMM_MODE_NONE || handle->enabled) return ESP_ERR_INVALID_STATE;
    handle->enabled = true;
    const int64_t now = Now(handle);
    handle->readPosition = handle->queueEnd = handle->rxRead = now;
    handle->nextBoundary = now + handle->framesPerDescriptor;
    handle->dry = false;
    if (handle->tx && handle->preload != nullptr)
    {
        const uint32_t frames = static_cast<uint32_t>(handle->preload->size() / BytesPerFrame(handle));
        ToSpeaker(handle, handle->queueEnd, handle->preload->data(), frames);
        handle->queueEnd += frames;
        handle->preload->clear();
    }
    Rearm(handle);
    return ESP_OK;
}

esp_err_t i2s_channel_disable(i2s_chan_handle_t handle)
{
    if (!Valid(handle)) return ESP_ERR_INVALID_ARG;
    if (!handle->enabled) return ESP_ERR_INVALID_STATE;
    handle->enabled = false;
    Rearm(handle);
    k::WakeAll(&handle->waiters);
    return ESP_OK;
}

esp_err_t i2s_channel_preload_data(i2s_chan_handle_t tx_handle, const void* src, size_t size, size_t* bytes_loaded)
{
    if (!Valid(tx_handle) || !tx_handle->tx || src == nullptr) return ESP_ERR_INVALID_ARG;
    if (tx_handle->enabled) return ESP_ERR_INVALID_STATE;
    if (tx_handle->preload == nullptr) tx_handle->preload = new std::vector<uint8_t>();
    const size_t room = Capacity(tx_handle) * BytesPerFrame(tx_handle) - tx_handle->preload->size();
    const size_t take = std::min(room, size);
    const auto* bytes = static_cast<const uint8_t*>(src);
    tx_handle->preload->insert(tx_handle->preload->end(), bytes, bytes + take);
    if (bytes_loaded != nullptr) *bytes_loaded = take;
    return ESP_OK;
}

esp_err_t i2s_channel_write(i2s_chan_handle_t handle, const void* src, size_t size, size_t* bytes_written, uint32_t timeout_ms)
{
    if (bytes_written != nullptr) *bytes_written = 0;
    if (!Valid(handle) || !handle->tx || src == nullptr) return ESP_ERR_INVALID_ARG;
    if (!handle->enabled) return ESP_ERR_INVALID_STATE;
    const uint32_t frameBytes = BytesPerFrame(handle);
    const uint32_t frames = static_cast<uint32_t>(size / frameBytes);
    const auto* bytes = static_cast<const uint8_t*>(src);
    const uint64_t deadline = timeout_ms == portMAX_DELAY ? k::kForever : k::NowUs() + static_cast<uint64_t>(timeout_ms) * 1000;
    uint32_t done = 0;
    for (;;)
    {
        AdvanceTx(handle);
        const int64_t queued = handle->queueEnd - handle->readPosition;
        const int64_t room = static_cast<int64_t>(Capacity(handle)) - queued;
        if (room > 0 && done < frames)
        {
            const uint32_t count = static_cast<uint32_t>(std::min<int64_t>(room, frames - done));
            ToSpeaker(handle, handle->queueEnd, bytes + static_cast<size_t>(done) * frameBytes, count);
            handle->queueEnd += count;
            handle->dry = false;
            done += count;
        }
        if (done == frames || !handle->enabled || !k::InTask() || k::NowUs() >= deadline) break;
        // The DMA frees a buffer at the next boundary.
        k::Block(&handle->waiters, std::min<uint64_t>(deadline, DueUs(handle, handle->nextBoundary)));
    }
    if (bytes_written != nullptr) *bytes_written = static_cast<size_t>(done) * frameBytes;
    k::PreemptionPoint();
    return done == frames ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t i2s_channel_read(i2s_chan_handle_t handle, void* dest, size_t size, size_t* bytes_read, uint32_t timeout_ms)
{
    if (bytes_read != nullptr) *bytes_read = 0;
    if (!Valid(handle) || handle->tx || dest == nullptr) return ESP_ERR_INVALID_ARG;
    if (!handle->enabled) return ESP_ERR_INVALID_STATE;
    const uint32_t frameBytes = BytesPerFrame(handle);
    const uint32_t frames = static_cast<uint32_t>(size / frameBytes);
    auto* out = static_cast<uint8_t*>(dest);
    const uint64_t deadline = timeout_ms == portMAX_DELAY ? k::kForever : k::NowUs() + static_cast<uint64_t>(timeout_ms) * 1000;
    uint32_t done = 0;
    for (;;)
    {
        const int64_t available = AvailableRx(handle);
        if (available > 0 && done < frames)
        {
            const uint32_t count = static_cast<uint32_t>(std::min<int64_t>(available, frames - done));
            int16_t mono[512];
            for (uint32_t at = 0; at < count; at += 512)
            {
                const uint32_t chunk = std::min<uint32_t>(512, count - at);
                if (HearsMicrophone(handle)) a::Microphone(handle->rxRead + at, mono, chunk);
                else std::memset(mono, 0, sizeof(mono));
                for (uint32_t index = 0; index < chunk; ++index)
                {
                    uint8_t* frame = out + static_cast<size_t>(done + at + index) * frameBytes;
                    for (uint32_t slot = 0; slot < handle->channels; ++slot)
                    {
                        uint8_t* sample = frame + slot * BytesPerSample(handle);
                        const int32_t value = mono[index];
                        switch (BytesPerSample(handle))
                        {
                            case 1: sample[0] = static_cast<uint8_t>(value >> 8); break;
                            case 2: std::memcpy(sample, &mono[index], 2); break;
                            case 3:
                                sample[0] = 0;
                                sample[1] = static_cast<uint8_t>(value);
                                sample[2] = static_cast<uint8_t>(value >> 8);
                                break;
                            default:
                            {
                                const int32_t wide = value << 16;
                                std::memcpy(sample, &wide, 4);
                            }
                        }
                    }
                }
            }
            handle->rxRead += count;
            done += count;
        }
        if (done == frames || !handle->enabled || !k::InTask() || k::NowUs() >= deadline) break;
        const int64_t needed = std::min<int64_t>(frames - done, handle->framesPerDescriptor);
        k::Block(&handle->waiters, std::min<uint64_t>(deadline, DueUs(handle, handle->rxRead + needed)));
    }
    if (bytes_read != nullptr) *bytes_read = static_cast<size_t>(done) * frameBytes;
    k::PreemptionPoint();
    return done == frames ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t i2s_channel_register_event_callback(i2s_chan_handle_t handle, const i2s_event_callbacks_t* callbacks, void* user_data)
{
    if (!Valid(handle) || callbacks == nullptr) return ESP_ERR_INVALID_ARG;
    if (handle->enabled) return ESP_ERR_INVALID_STATE;
    handle->callbacks = *callbacks;
    handle->context = user_data;
    return ESP_OK;
}

esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t* std_cfg)
{
    if (!Valid(handle) || std_cfg == nullptr) return ESP_ERR_INVALID_ARG;
    if (handle->mode != I2S_COMM_MODE_NONE) return ESP_ERR_INVALID_STATE;
    handle->mode = I2S_COMM_MODE_STD;
    handle->rate = std_cfg->clk_cfg.sample_rate_hz != 0 ? std_cfg->clk_cfg.sample_rate_hz : a::kRate;
    ApplyStdSlot(handle, &std_cfg->slot_cfg);
    ApplyStdGpio(handle, &std_cfg->gpio_cfg);
    ChargeDma(handle);
    return handle->dmaCharge != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t i2s_channel_reconfig_std_clock(i2s_chan_handle_t handle, const i2s_std_clk_config_t* clk_cfg)
{
    if (!Valid(handle) || clk_cfg == nullptr || clk_cfg->sample_rate_hz == 0) return ESP_ERR_INVALID_ARG;
    if (handle->enabled) return ESP_ERR_INVALID_STATE;
    handle->rate = clk_cfg->sample_rate_hz;
    return ESP_OK;
}

esp_err_t i2s_channel_reconfig_std_slot(i2s_chan_handle_t handle, const i2s_std_slot_config_t* slot_cfg)
{
    if (!Valid(handle) || slot_cfg == nullptr) return ESP_ERR_INVALID_ARG;
    if (handle->enabled) return ESP_ERR_INVALID_STATE;
    ApplyStdSlot(handle, slot_cfg);
    return ESP_OK;
}

esp_err_t i2s_channel_reconfig_std_gpio(i2s_chan_handle_t handle, const i2s_std_gpio_config_t* gpio_cfg)
{
    if (!Valid(handle) || gpio_cfg == nullptr) return ESP_ERR_INVALID_ARG;
    if (handle->enabled) return ESP_ERR_INVALID_STATE;
    ApplyStdGpio(handle, gpio_cfg);
    return ESP_OK;
}

esp_err_t i2s_channel_init_pdm_rx_mode(i2s_chan_handle_t handle, const i2s_pdm_rx_config_t* cfg)
{
    if (!Valid(handle) || cfg == nullptr || handle->tx) return ESP_ERR_INVALID_ARG;
    if (handle->mode != I2S_COMM_MODE_NONE) return ESP_ERR_INVALID_STATE;
    handle->mode = I2S_COMM_MODE_PDM;
    handle->rate = cfg->clk_cfg.sample_rate_hz != 0 ? cfg->clk_cfg.sample_rate_hz : a::kRate;
    handle->bits = cfg->slot_cfg.data_bit_width;
    handle->channels = cfg->slot_cfg.slot_mode == I2S_SLOT_MODE_MONO ? 1 : 2;
    ApplyPdmGpio(handle, &cfg->gpio_cfg);
    ChargeDma(handle);
    return handle->dmaCharge != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t i2s_channel_reconfig_pdm_rx_clock(i2s_chan_handle_t handle, const i2s_pdm_rx_clk_config_t* clk_cfg)
{
    if (!Valid(handle) || clk_cfg == nullptr || clk_cfg->sample_rate_hz == 0) return ESP_ERR_INVALID_ARG;
    if (handle->enabled) return ESP_ERR_INVALID_STATE;
    handle->rate = clk_cfg->sample_rate_hz;
    return ESP_OK;
}

esp_err_t i2s_channel_reconfig_pdm_rx_slot(i2s_chan_handle_t handle, const i2s_pdm_rx_slot_config_t* slot_cfg)
{
    if (!Valid(handle) || slot_cfg == nullptr) return ESP_ERR_INVALID_ARG;
    if (handle->enabled) return ESP_ERR_INVALID_STATE;
    handle->bits = slot_cfg->data_bit_width;
    handle->channels = slot_cfg->slot_mode == I2S_SLOT_MODE_MONO ? 1 : 2;
    return ESP_OK;
}

esp_err_t i2s_channel_reconfig_pdm_rx_gpio(i2s_chan_handle_t handle, const i2s_pdm_rx_gpio_config_t* gpio_cfg)
{
    if (!Valid(handle) || gpio_cfg == nullptr) return ESP_ERR_INVALID_ARG;
    if (handle->enabled) return ESP_ERR_INVALID_STATE;
    ApplyPdmGpio(handle, gpio_cfg);
    return ESP_OK;
}

}  // extern "C"

namespace cardenza_web::audio {

bool MicrophoneWanted()
{
    for (i2s_channel_obj_t* channel : g_rx)
        if (channel != nullptr && channel->enabled && HearsMicrophone(channel)) return true;
    return false;
}

}  // namespace cardenza_web::audio
