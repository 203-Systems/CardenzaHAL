#include "system.h"
#include "../audio/codec/es8156.h"
#include "internal/hooks.h"
#include "internal/nvs_access.h"
#include "../board/pins.h"

#include <driver/i2c_master.h>
#include <atomic>

#include <driver/gpio.h>

namespace CardenzaHAL::System {

namespace {

i2c_master_bus_handle_t i2cBus = nullptr;

bool InitializeI2CBus()
{
    if (i2cBus != nullptr)
    {
        return true;
    }

    i2c_master_bus_config_t busConfig = {};
    busConfig.i2c_port = static_cast<i2c_port_num_t>(Pins::kExternalI2cPort);
    busConfig.scl_io_num = static_cast<gpio_num_t>(Pins::kExternalI2cScl);
    busConfig.sda_io_num = static_cast<gpio_num_t>(Pins::kExternalI2cSda);
    busConfig.clk_source = I2C_CLK_SRC_DEFAULT;
    busConfig.glitch_ignore_cnt = 7;
    busConfig.flags.enable_internal_pullup = true;

    return i2c_new_master_bus(&busConfig, &i2cBus) == ESP_OK;
}

bool ConfigureHomeButton()
{
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << Pins::kHomeButton;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config) == ESP_OK;
}

}  // namespace

bool Init()
{
    return InitializeI2CBus() && ConfigureHomeButton();
}

void EnterDeepSleep()
{
    if (i2cBus == nullptr) return;
    // ESP-IDF deletes a bus only with no device on it: the HAL's codec goes first. Until then the handle
    // stays, so a bus that could not be deleted is not lost.
    Audio::Codec::ES8156::Release();
    if (i2c_del_master_bus(i2cBus) == ESP_OK) i2cBus = nullptr;
}

i2c_master_bus_handle_t GetI2CBus()
{
    return i2cBus;
}

namespace {

// The system menu's hold on the side key (system/menu.h): 0 none, 1 the menu has it (the firmware reads
// it released), 2 hidden until it is let go (the press that opened or closed the menu).
std::atomic<uint8_t> funcHold{0};
std::atomic<void (*)()> usbStop{nullptr};
std::atomic<bool> homeInjected{false};  // HOME held from a computer (detail::InjectHome)

}  // namespace

bool GetFuncButton()
{
    const bool down = detail::HomeButtonRaw();
    const uint8_t hold = funcHold.load();
    if (hold == 1) return false;
    if (hold == 2)
    {
        if (down) return false;
        uint8_t expected = 2;
        funcHold.compare_exchange_strong(expected, 0);
    }
    return down;
}

namespace detail {

bool HomeButtonRaw()
{
    return homeInjected.load(std::memory_order_relaxed) || gpio_get_level(static_cast<gpio_num_t>(Pins::kHomeButton)) == 0;
}

void InjectHome(bool down)
{
    homeInjected.store(down, std::memory_order_relaxed);
}

void RestoreHomeButton()
{
    (void)ConfigureHomeButton();
}

void HoldHomeButton(bool held)
{
    funcHold.store(held ? 1 : 2);
}

void SetUsbStop(void (*stop)())
{
    usbStop.store(stop);
}

void StopUsb()
{
    if (auto stop = usbStop.exchange(nullptr)) stop();
}

}  // namespace detail

}  // namespace CardenzaHAL::System

namespace CardenzaHAL::System::detail {

std::mutex& NvsWriteMutex()
{
    static std::mutex mutex;
    return mutex;
}

}  // namespace CardenzaHAL::System::detail
