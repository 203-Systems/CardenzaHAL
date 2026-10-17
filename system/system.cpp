#include "system.h"
#include "../board/pins.h"

#include <driver/i2c_master.h>
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

bool ConfigureFuncButton()
{
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << Pins::kFuncButton;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config) == ESP_OK;
}

}  // namespace

bool Init()
{
    return InitializeI2CBus() && ConfigureFuncButton();
}

void EnterDeepSleep()
{
    if (i2cBus != nullptr)
    {
        i2c_del_master_bus(i2cBus);
        i2cBus = nullptr;
    }
}

i2c_master_bus_handle_t GetI2CBus()
{
    return i2cBus;
}

bool GetFuncButton()
{
    return gpio_get_level(static_cast<gpio_num_t>(Pins::kFuncButton)) == 0;
}

}  // namespace CardenzaHAL::System
