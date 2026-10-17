#include "keyboard_hardware.h"

#include "keyboard_driver.h"
#include "../layout.h"

#include <esp_private/periph_ctrl.h>
#include <esp_rom_gpio.h>
#include <hal/spi_ll.h>

namespace CardenzaHAL::Keyboard::detail {

namespace {

constexpr spi_host_device_t kKeyboardLedSpiHost = SPI2_HOST;
constexpr int kKeyboardLedSpiSourceHz = 80 * 1000 * 1000;
constexpr int kKeyboardLedSpiClockHz = 10 * 1000 * 1000;

const uint64_t kKeyboardOutputMask = PinMask(output_list[0]) | PinMask(output_list[1]) | PinMask(output_list[2]);

}  // namespace

portMUX_TYPE kKeyboardStateMux = portMUX_INITIALIZER_UNLOCKED;
spi_dev_t* gKeyboardLedSpiHw = nullptr;
bool gKeyboardLedSpiReady = false;

void FastWriteMask(uint64_t setMask, uint64_t clearMask)
{
    const uint32_t set_low = static_cast<uint32_t>(setMask);
    const uint32_t clear_low = static_cast<uint32_t>(clearMask);
    if (clear_low != 0)
    {
        GPIO.out_w1tc = clear_low;
    }
    if (set_low != 0)
    {
        GPIO.out_w1ts = set_low;
    }

    const uint32_t set_high = static_cast<uint32_t>(setMask >> 32);
    const uint32_t clear_high = static_cast<uint32_t>(clearMask >> 32);
    if (clear_high != 0)
    {
        GPIO.out1_w1tc.data = clear_high;
    }
    if (set_high != 0)
    {
        GPIO.out1_w1ts.data = set_high;
    }
}

void FastWritePin(int pin, bool level)
{
    const uint64_t mask = PinMask(pin);
    FastWriteMask(level ? mask : 0, level ? 0 : mask);
}

int FastReadPin(int pin)
{
    if (pin < 32)
    {
        return (GPIO.in >> pin) & 0x1;
    }

    return (GPIO.in1.data >> (pin - 32)) & 0x1;
}

void InitKeyboardLedSpi()
{
    PERIPH_RCC_ATOMIC()
    {
        spi_ll_enable_bus_clock(kKeyboardLedSpiHost, true);
        spi_ll_reset_register(kKeyboardLedSpiHost);
    }

    spi_dev_t* const hw = spi_periph_signal[kKeyboardLedSpiHost].hw;
    spi_ll_enable_clock(kKeyboardLedSpiHost, true);
    spi_ll_master_init(hw);
    spi_ll_set_clk_source(hw, SPI_CLK_SRC_APB);
    spi_ll_master_set_clock(hw, kKeyboardLedSpiSourceHz, kKeyboardLedSpiClockHz, 128);
    spi_ll_master_set_mode(hw, 0);
    spi_ll_set_half_duplex(hw, true);
    spi_ll_set_sio_mode(hw, 0);
    spi_ll_set_tx_lsbfirst(hw, false);
    spi_ll_set_rx_lsbfirst(hw, false);
    spi_ll_master_select_cs(hw, -1);
    spi_ll_master_keep_cs(hw, 0);
    spi_ll_master_set_cs_setup(hw, 0);
    spi_ll_master_set_cs_hold(hw, 0);
    spi_ll_set_command_bitlen(hw, 0);
    spi_ll_set_addr_bitlen(hw, 0);
    spi_ll_set_dummy(hw, 0);
    spi_ll_enable_miso(hw, 0);
    spi_ll_enable_mosi(hw, 1);
    spi_ll_set_mosi_bitlen(hw, 8);
    spi_ll_set_mosi_free_level(hw, false);
    spi_ll_cpu_tx_fifo_reset(hw);
    spi_ll_cpu_rx_fifo_reset(hw);
    spi_ll_apply_config(hw);

    const spi_signal_conn_t& spi_signal = spi_periph_signal[kKeyboardLedSpiHost];
    esp_rom_gpio_pad_select_gpio(led_pin_list[1]);
    esp_rom_gpio_pad_select_gpio(led_pin_list[2]);
    esp_rom_gpio_connect_out_signal(led_pin_list[1], spi_signal.spiclk_out, false, false);
    esp_rom_gpio_connect_out_signal(led_pin_list[2], spi_signal.spid_out, false, false);

    gKeyboardLedSpiHw = hw;
    gKeyboardLedSpiReady = true;
}

bool KeyboardScanTimerCallback(gptimer_handle_t,
                               const gptimer_alarm_event_data_t*,
                               void* userContext)
{
    return static_cast<KeyboardDriver*>(userContext)->handleTimerTickFromIsr();
}

uint64_t KeyboardOutputMask()
{
    return kKeyboardOutputMask;
}

}  // namespace CardenzaHAL::Keyboard::detail
