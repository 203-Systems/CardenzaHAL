#include "display.h"
#include "backlight_brightness.h"
#include "../board/pins.h"

#include <algorithm>
#include <cstring>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_attr.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_commands.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace CardenzaHAL::Display {

namespace {

esp_lcd_panel_handle_t panel = nullptr;
esp_lcd_panel_io_handle_t panelIo = nullptr;
uint8_t brightness = 255;

constexpr spi_host_device_t kDisplaySpiHost = SPI3_HOST;
constexpr ledc_mode_t kBacklightSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_7;
constexpr ledc_timer_bit_t kBacklightResolution = LEDC_TIMER_12_BIT;
// S3 low-speed LEDC timers share their clock source with keyboard OE PWM.
// APB 80 MHz supports 1 kHz with 12-bit duty for smooth low-brightness steps.
constexpr uint32_t kBacklightFrequencyHz = 1000;

bool spiBusInitialized = false;
bool backlightInitialized = false;
bool displayInitialized = false;
bool backlightDutyApplied = false;
uint16_t appliedBacklightDuty = 0;
bool startupPolicySelected = false;
bool preservePixels = false;
bool firstFramePresented = false;
bool fullRefreshRequired = true;
constexpr int kTransferBandRows = 8;
constexpr uint32_t kTransferTimeoutMs = 20;
DMA_ATTR uint16_t transferBand[WIDTH * kTransferBandRows] = {};
SemaphoreHandle_t transferDone = nullptr;
bool completionCallbackInstalled = false;
bool transferPending = false;

esp_err_t ApplyBrightness(uint8_t value)
{
    const uint16_t duty = detail::BacklightDuty(value);
    if (backlightDutyApplied && appliedBacklightDuty == duty) return ESP_OK;
    const esp_err_t dutyStatus = ledc_set_duty(kBacklightSpeedMode, kBacklightChannel, duty);
    if (dutyStatus != ESP_OK)
    {
        return dutyStatus;
    }

    const esp_err_t updateStatus = ledc_update_duty(kBacklightSpeedMode, kBacklightChannel);
    if (updateStatus == ESP_OK)
    {
        appliedBacklightDuty = duty;
        backlightDutyApplied = true;
    }
    return updateStatus;
}

esp_err_t InitBacklight(uint8_t initialBrightness)
{
    if (backlightInitialized)
    {
        return ESP_OK;
    }

    ledc_timer_config_t timerConfig = {};
    timerConfig.speed_mode = kBacklightSpeedMode;
    timerConfig.duty_resolution = kBacklightResolution;
    timerConfig.timer_num = kBacklightTimer;
    timerConfig.freq_hz = kBacklightFrequencyHz;
    timerConfig.clk_cfg = LEDC_USE_APB_CLK;

    esp_err_t status = ledc_timer_config(&timerConfig);
    if (status != ESP_OK)
    {
        return status;
    }

    ledc_channel_config_t channelConfig = {};
    channelConfig.gpio_num = Pins::kDisplayBacklight;
    channelConfig.speed_mode = kBacklightSpeedMode;
    channelConfig.channel = kBacklightChannel;
    channelConfig.intr_type = LEDC_INTR_DISABLE;
    channelConfig.timer_sel = kBacklightTimer;
    channelConfig.duty = detail::BacklightDuty(initialBrightness);
    channelConfig.hpoint = 0;

    status = ledc_channel_config(&channelConfig);
    if (status != ESP_OK)
    {
        return status;
    }

    backlightInitialized = true;
    appliedBacklightDuty = channelConfig.duty;
    backlightDutyApplied = true;
    return ESP_OK;
}

esp_err_t InitSpiBus()
{
    if (spiBusInitialized)
    {
        return ESP_OK;
    }

    spi_bus_config_t busConfig = {};
    busConfig.mosi_io_num = Pins::kDisplayMosi;
    busConfig.miso_io_num = Pins::kDisplayMiso;
    busConfig.sclk_io_num = Pins::kDisplaySclk;
    busConfig.quadwp_io_num = -1;
    busConfig.quadhd_io_num = -1;
    busConfig.max_transfer_sz = WIDTH * HEIGHT * static_cast<int>(sizeof(uint16_t));

    const esp_err_t status = spi_bus_initialize(kDisplaySpiHost, &busConfig, SPI_DMA_CH_AUTO);
    if (status != ESP_OK)
    {
        return status;
    }

    spiBusInitialized = true;
    return ESP_OK;
}

bool IRAM_ATTR OnTransferDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* context)
{
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(context), &wake);
    return wake == pdTRUE;
}

bool EnsureCompletionCallback()
{
    if (completionCallbackInstalled) return true;
    if (transferDone == nullptr) transferDone = xSemaphoreCreateBinary();
    if (transferDone == nullptr) return false;
    esp_lcd_panel_io_callbacks_t callbacks = {};
    callbacks.on_color_trans_done = OnTransferDone;
    if (esp_lcd_panel_io_register_event_callbacks(panelIo, &callbacks, transferDone) != ESP_OK) return false;
    completionCallbackInstalled = true;
    return true;
}

bool WaitForTransfer(TickType_t timeout)
{
    if (!transferPending) return true;
    if (xSemaphoreTake(transferDone, timeout) != pdTRUE) return false;
    transferPending = false;
    return true;
}

bool RevealFrame()
{
    if (!firstFramePresented)
    {
        if (esp_lcd_panel_disp_on_off(panel, true) != ESP_OK) return false;
        // A warm start leaves the previous backlight route alone until now.
        // Configure our PWM at its final duty before releasing any pad hold.
        if (InitBacklight(brightness) != ESP_OK) return false;
    }
    if (ApplyBrightness(brightness) != ESP_OK) return false;
    if (!firstFramePresented && gpio_hold_dis(static_cast<gpio_num_t>(Pins::kDisplayBacklight)) != ESP_OK) return false;
    firstFramePresented = true;
    return true;
}

bool ValidRegion(const Region& region)
{
    return region.x >= 0 && region.y >= 0 && region.x < WIDTH && region.y < HEIGHT &&
           region.width > 0 && region.height > 0 &&
           region.width <= WIDTH - region.x && region.height <= HEIGHT - region.y;
}

bool SendRegion(const uint16_t* framebuffer, const Region& region)
{
    for (int y = region.y; y < region.y + region.height; y += kTransferBandRows)
    {
        const int rows = std::min(kTransferBandRows, region.y + region.height - y);
        for (int row = 0; row < rows; ++row)
            std::memcpy(transferBand + row * region.width, framebuffer + (y + row) * WIDTH + region.x,
                        static_cast<std::size_t>(region.width) * sizeof(uint16_t));
        while (xSemaphoreTake(transferDone, 0) == pdTRUE) {}
        if (esp_lcd_panel_draw_bitmap(panel, region.x, y, region.x + region.width, y + rows, transferBand) != ESP_OK)
            return false;
        transferPending = true;
        if (!WaitForTransfer(pdMS_TO_TICKS(kTransferTimeoutMs))) return false;
    }
    return true;
}

}  // namespace

bool Init()
{
    if (displayInitialized) return true;
    if (!startupPolicySelected)
    {
        // Launcher boots applications through a software restart. Preserve the
        // controller's RAM on that path; panel init below still wakes it and
        // restores the pixel format if the previous owner left it asleep.
        preservePixels = esp_reset_reason() == ESP_RST_SW;
        startupPolicySelected = true;
    }
    esp_err_t status = ESP_OK;
    if (preservePixels)
    {
        // Set the latch before enabling output: never pulse the LCD reset low.
        if (gpio_set_level(static_cast<gpio_num_t>(Pins::kDisplayReset), 1) != ESP_OK ||
            gpio_set_direction(static_cast<gpio_num_t>(Pins::kDisplayReset), GPIO_MODE_OUTPUT) != ESP_OK) return false;
    }
    else
    {
        if (InitBacklight(0) != ESP_OK ||
            gpio_hold_dis(static_cast<gpio_num_t>(Pins::kDisplayBacklight)) != ESP_OK) return false;
    }
    status = InitSpiBus();
    if (status != ESP_OK)
    {
        return false;
    }

    if (panelIo == nullptr)
    {
        esp_lcd_panel_io_spi_config_t ioConfig = {};
        ioConfig.cs_gpio_num = Pins::kDisplayCs;
        ioConfig.dc_gpio_num = Pins::kDisplayDc;
        ioConfig.spi_mode = 0;
        ioConfig.pclk_hz = 40 * 1000 * 1000;
        ioConfig.trans_queue_depth = 10;
        ioConfig.lcd_cmd_bits = 8;
        ioConfig.lcd_param_bits = 8;

        status = esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kDisplaySpiHost), &ioConfig, &panelIo);
        if (status != ESP_OK)
        {
            return false;
        }
    }

    if (panel == nullptr)
    {
        esp_lcd_panel_dev_config_t panelConfig = {};
        panelConfig.reset_gpio_num = preservePixels ? -1 : Pins::kDisplayReset;
        panelConfig.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panelConfig.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
        panelConfig.bits_per_pixel = 16;

        status = esp_lcd_new_panel_st7789(panelIo, &panelConfig, &panel);
        if (status != ESP_OK)
        {
            return false;
        }
    }

    // A handle alone does not mean reset/configuration succeeded. Retry every
    // configuration stage after a previous failure, keeping allocated handles.
    if (!preservePixels && esp_lcd_panel_reset(panel) != ESP_OK) return false;

    status = esp_lcd_panel_init(panel);
    if (status != ESP_OK)
    {
        return false;
    }

    status = esp_lcd_panel_invert_color(panel, true);
    if (status != ESP_OK)
    {
        return false;
    }

    status = esp_lcd_panel_swap_xy(panel, true);
    if (status != ESP_OK)
    {
        return false;
    }

    status = esp_lcd_panel_mirror(panel, true, false);
    if (status != ESP_OK)
    {
        return false;
    }

    status = esp_lcd_panel_set_gap(panel, Pins::kDisplayLandscapeGapX, Pins::kDisplayLandscapeGapY);
    if (status != ESP_OK)
    {
        return false;
    }

    if (!preservePixels && esp_lcd_panel_disp_on_off(panel, false) != ESP_OK) return false;
    displayInitialized = true;
    return true;
}

bool Present(const uint16_t* framebuffer)
{
    const Region fullFrame{0, 0, WIDTH, HEIGHT};
    return Present(framebuffer, &fullFrame, 1);
}

bool Present(const uint16_t* framebuffer, const Region* regions, std::size_t count)
{
    if (framebuffer == nullptr || (count != 0 && regions == nullptr)) return false;
    for (std::size_t index = 0; index < count; ++index)
        if (!ValidRegion(regions[index])) return false;
    if (!Init() || !EnsureCompletionCallback()) return false;
    // A timed-out DMA still owns transferBand. Never overwrite it while the
    // caller continues rendering into its own framebuffer.
    if (!WaitForTransfer(0)) return false;
    bool sent = true;
    if (fullRefreshRequired)
        sent = SendRegion(framebuffer, Region{0, 0, WIDTH, HEIGHT});
    else
        for (std::size_t index = 0; sent && index < count; ++index)
            sent = SendRegion(framebuffer, regions[index]);
    if (!sent || !RevealFrame())
    {
        fullRefreshRequired = true;
        return false;
    }
    fullRefreshRequired = false;
    return true;
}

bool Present()
{
    if (!Init() || !WaitForTransfer(0)) return false;
    // Raw-handle users own submission and callbacks. The SDK parameter
    // transaction synchronously drains their preceding color transfers.
    if (esp_lcd_panel_io_tx_param(panelIo, LCD_CMD_NOP, nullptr, 0) != ESP_OK) return false;
    if (!RevealFrame()) return false;
    fullRefreshRequired = false;
    return true;
}

esp_lcd_panel_handle_t GetPanel()
{
    return panel;
}

esp_lcd_panel_io_handle_t GetPanelIO()
{
    return panelIo;
}

bool SetBrightness(uint8_t value)
{
    brightness = value;

    if (!firstFramePresented)
    {
        return true;
    }

    return ApplyBrightness(value) == ESP_OK;
}

uint8_t GetBrightness()
{
    return brightness;
}

}  // namespace CardenzaHAL::Display
