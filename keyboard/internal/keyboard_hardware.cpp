#include "keyboard_hardware.h"

#include "keyboard_driver.h"
#include "../layout.h"

#include <esp_private/periph_ctrl.h>
#include <esp_rom_gpio.h>
#include <hal/spi_ll.h>
#include <driver/ledc.h>
#include <esp_check.h>
#include <soc/gpio_sig_map.h>
#include <soc/ledc_periph.h>

namespace CardenzaHAL::Keyboard::detail {

namespace {

// Display uses LEDC timer 0 / channel 7. Reserve timer 1 / channel 6 for OE.
constexpr ledc_timer_t kLedOeTimer = LEDC_TIMER_1;
constexpr ledc_channel_t kLedOeChannel = LEDC_CHANNEL_6;
bool ledPwmReady = false;
constexpr spi_host_device_t kKeyboardLedSpiHost = SPI2_HOST;
constexpr int kKeyboardLedSpiSourceHz = 80 * 1000 * 1000;
constexpr int kKeyboardLedSpiClockHz = 10 * 1000 * 1000;

const uint64_t kKeyboardOutputMask = PinMask(output_list[0]) | PinMask(output_list[1]) | PinMask(output_list[2]);

}  // namespace

portMUX_TYPE kKeyboardStateMux = portMUX_INITIALIZER_UNLOCKED;
spi_dev_t* gKeyboardLedSpiHw = nullptr;

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
}

void GateKeyboardLeds(bool active)
{
    // A GPIO latch write alone cannot blank a pin routed to LEDC. Disconnect
    // PWM during row/column transitions and restore the inverted OE signal.
    FastWritePin(led_pin_list[0], true);
    if (active && ledPwmReady)
    {
        esp_rom_gpio_connect_out_signal(led_pin_list[0],
            ledc_periph_signal[LEDC_LOW_SPEED_MODE].sig_out0_idx + kLedOeChannel,
            true, false);
    }
    else
    {
        esp_rom_gpio_connect_out_signal(led_pin_list[0], SIG_GPIO_OUT_IDX, false, false);
    }
}

esp_err_t InitKeyboardLedPwm()
{
    if (ledPwmReady) return ESP_OK;
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.timer_num = kLedOeTimer;
    timer.freq_hz = 78125; // APB 80 MHz / 1024; 12.8 us period.
    timer.clk_cfg = LEDC_USE_APB_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), "CardenzaKeyboard", "OE timer failed");
    ledc_channel_config_t channel = {};
    channel.gpio_num = led_pin_list[0];
    channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = kLedOeChannel;
    channel.timer_sel = kLedOeTimer;
    channel.duty = 0;
    channel.hpoint = 0;
    channel.flags.output_invert = 1; // LED_EN is active low.
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), "CardenzaKeyboard", "OE PWM failed");
    ledPwmReady = true;
    GateKeyboardLeds(false);
    return ESP_OK;
}

esp_err_t SetKeyboardLedGlobalDuty(uint16_t duty)
{
    if (!ledPwmReady) return ESP_OK; // Cached app setting is applied during init.
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, kLedOeChannel, duty), "CardenzaKeyboard", "OE duty failed");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, kLedOeChannel);
}

bool KeyboardScanTimerCallback(gptimer_handle_t timer,
                               const gptimer_alarm_event_data_t*,
                               void* userContext)
{
    auto* driver = static_cast<KeyboardDriver*>(userContext);
    const bool wake = driver->handleTimerTickFromIsr();
    // One-shot alarms allow binary-weighted dwell without raising the shortest
    // period's interrupt rate for the long planes. Schedule from current time
    // so a delayed ISR cannot arm an alarm that has already expired.
    uint64_t now = 0;
    gptimer_get_raw_count(timer, &now);
    gptimer_alarm_config_t alarm = {};
    alarm.alarm_count = now + driver->nextTimerTicks();
    gptimer_set_alarm_action(timer, &alarm);
    return wake;
}

uint64_t KeyboardOutputMask()
{
    return kKeyboardOutputMask;
}

}  // namespace CardenzaHAL::Keyboard::detail
