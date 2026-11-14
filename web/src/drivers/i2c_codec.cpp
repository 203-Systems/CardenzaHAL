// The I2C master driver on the web target, and the one device on the board's bus: the ES8156 audio
// codec at 0x08, as a register file that answers its identity, reports its state machine running
// and turns its volume register into the speaker's gain. Transfers take the time their bits need at
// the device's SCL speed (the HAL talks to the codec at 10 kHz: its startup takes as long as on the
// board). Any other address does not acknowledge.

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_state.h"
#include "../kernel/kernel.h"

namespace k = cardenza_web::kernel;

struct i2c_master_bus_t {
    int port;
    bool board;  // SCL 1 / SDA 2: the codec is on it
    int devices;
};

struct i2c_master_dev_t {
    i2c_master_bus_t* bus;
    uint16_t address;
    uint32_t speed;
};

namespace {

constexpr uint16_t kCodecAddress = 0x08;
constexpr int kBoardScl = 1;
constexpr int kBoardSda = 2;

i2c_master_bus_t* g_buses[2] = {};

struct Es8156 {
    uint8_t registers[256];
    Es8156()
    {
        std::memset(registers, 0, sizeof(registers));
        registers[0xFD] = 0x81;
        registers[0xFE] = 0x55;
        registers[0xFF] = 0x20;
    }
    bool Running() const { return (registers[0x00] & 0x03) == 0x03 && registers[0x25] != 0xA1; }
    uint8_t Read(uint8_t reg) const
    {
        if (reg == 0x0C) return Running() ? 0x60 : 0x00;  // the state machine's "normal" state
        return registers[reg];
    }
    void Write(uint8_t reg, uint8_t value) { registers[reg] = value; }
};

Es8156& Codec()
{
    static Es8156 codec;
    return codec;
}

bool Acknowledges(const i2c_master_dev_t* device)
{
    return device->bus->board && device->address == kCodecAddress;
}

// The bus time of a transfer: a start, the address and each byte with its acknowledge bit, a stop.
void SpendBusTime(const i2c_master_dev_t* device, size_t bytes, bool repeatedStart)
{
    const uint32_t speed = device->speed != 0 ? device->speed : 100000;
    const uint64_t bits = (1 + bytes + (repeatedStart ? 1 : 0)) * 9 + 2;
    const uint64_t micros = bits * 1000000u / speed;
    if (k::InTask()) k::SleepUntil(k::NowUs() + micros);
}

}  // namespace

namespace cardenza_web::audio {

float CodecGain()
{
    const Es8156& codec = Codec();
    if (!codec.Running()) return 0.0f;
    const uint8_t volume = codec.registers[0x14];
    if (volume == 0) return 0.0f;
    // 0.5 dB a step, 0xBF is 0 dB.
    return std::pow(10.0f, (static_cast<float>(volume) - 191.0f) * 0.5f / 20.0f);
}

bool CodecRunning() { return Codec().Running(); }

}  // namespace cardenza_web::audio

extern "C" {

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t* config, i2c_master_bus_handle_t* ret_bus_handle)
{
    if (config == nullptr || ret_bus_handle == nullptr) return ESP_ERR_INVALID_ARG;
    int port = config->i2c_port;
    if (port == -1)
    {
        port = g_buses[0] == nullptr ? 0 : (g_buses[1] == nullptr ? 1 : -1);
        if (port < 0) return ESP_ERR_NOT_FOUND;
    }
    if (port < 0 || port > 1) return ESP_ERR_INVALID_ARG;
    if (g_buses[port] != nullptr) return ESP_ERR_INVALID_STATE;
    auto* bus = static_cast<i2c_master_bus_t*>(std::calloc(1, sizeof(i2c_master_bus_t)));
    if (bus == nullptr) return ESP_ERR_NO_MEM;
    bus->port = port;
    bus->board = config->scl_io_num == kBoardScl && config->sda_io_num == kBoardSda;
    g_buses[port] = bus;
    *ret_bus_handle = bus;
    return ESP_OK;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t* config, i2c_master_dev_handle_t* ret_handle)
{
    if (bus == nullptr || config == nullptr || ret_handle == nullptr) return ESP_ERR_INVALID_ARG;
    auto* device = static_cast<i2c_master_dev_t*>(std::calloc(1, sizeof(i2c_master_dev_t)));
    if (device == nullptr) return ESP_ERR_NO_MEM;
    device->bus = bus;
    device->address = config->device_address;
    device->speed = config->scl_speed_hz;
    ++bus->devices;
    *ret_handle = device;
    return ESP_OK;
}

esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus)
{
    if (bus == nullptr) return ESP_ERR_INVALID_ARG;
    if (bus->devices != 0) return ESP_ERR_INVALID_STATE;
    g_buses[bus->port] = nullptr;
    std::free(bus);
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t handle)
{
    if (handle == nullptr) return ESP_ERR_INVALID_ARG;
    --handle->bus->devices;
    std::free(handle);
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t* data, size_t size, int)
{
    if (device == nullptr || (size != 0 && data == nullptr)) return ESP_ERR_INVALID_ARG;
    SpendBusTime(device, size, false);
    if (!Acknowledges(device)) return ESP_ERR_INVALID_STATE;  // NACK, as ESP-IDF 5.3 reports it
    if (size >= 1)
    {
        uint8_t reg = data[0];
        for (size_t index = 1; index < size; ++index) Codec().Write(reg++, data[index]);
    }
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t* write, size_t writeSize, uint8_t* read,
                                      size_t readSize, int)
{
    if (device == nullptr || read == nullptr) return ESP_ERR_INVALID_ARG;
    SpendBusTime(device, writeSize + readSize, true);
    if (!Acknowledges(device)) return ESP_ERR_INVALID_STATE;
    uint8_t reg = writeSize > 0 ? write[0] : 0;
    for (size_t index = 0; index < readSize; ++index) read[index] = Codec().Read(reg++);
    return ESP_OK;
}

esp_err_t i2c_master_receive(i2c_master_dev_handle_t device, uint8_t* read, size_t size, int)
{
    if (device == nullptr || read == nullptr) return ESP_ERR_INVALID_ARG;
    SpendBusTime(device, size, false);
    if (!Acknowledges(device)) return ESP_ERR_INVALID_STATE;
    std::memset(read, 0, size);
    return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int)
{
    if (bus == nullptr) return ESP_ERR_INVALID_ARG;
    return bus->board && address == kCodecAddress ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t i2c_master_execute_defined_operations(i2c_master_dev_handle_t device, i2c_operation_job_t* operations, size_t count, int timeout)
{
    if (device == nullptr || operations == nullptr) return ESP_ERR_INVALID_ARG;
    uint8_t reg = 0;
    bool haveReg = false;
    for (size_t index = 0; index < count; ++index)
    {
        i2c_operation_job_t& job = operations[index];
        if (job.command == I2C_MASTER_CMD_WRITE && job.write.total_bytes > 0)
        {
            const esp_err_t status = i2c_master_transmit(device, job.write.data, job.write.total_bytes, timeout);
            if (status != ESP_OK) return status;
            reg = job.write.data[0];
            haveReg = true;
        }
        else if (job.command == I2C_MASTER_CMD_READ)
        {
            const esp_err_t status = i2c_master_transmit_receive(device, &reg, haveReg ? 1 : 0, job.read.data, job.read.total_bytes, timeout);
            if (status != ESP_OK) return status;
        }
    }
    return ESP_OK;
}

esp_err_t i2c_master_register_event_callbacks(i2c_master_dev_handle_t, const i2c_master_event_callbacks_t*, void*) { return ESP_OK; }
esp_err_t i2c_master_bus_reset(i2c_master_bus_handle_t bus) { return bus == nullptr ? ESP_ERR_INVALID_ARG : ESP_OK; }
esp_err_t i2c_master_bus_wait_all_done(i2c_master_bus_handle_t bus, int) { return bus == nullptr ? ESP_ERR_INVALID_ARG : ESP_OK; }

esp_err_t i2c_master_get_bus_handle(i2c_port_num_t port_num, i2c_master_bus_handle_t* ret_handle)
{
    if (port_num < 0 || port_num > 1 || ret_handle == nullptr) return ESP_ERR_INVALID_ARG;
    if (g_buses[port_num] == nullptr) return ESP_ERR_INVALID_STATE;
    *ret_handle = g_buses[port_num];
    return ESP_OK;
}

}  // extern "C"
