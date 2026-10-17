#include "display.h"
#include "../board/pins.h"

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_attr.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_commands.h>
#include <esp_lcd_panel_vendor.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstring>

namespace CardenzaHAL::Display {

namespace {

esp_lcd_panel_handle_t panel = nullptr;
esp_lcd_panel_io_handle_t panelIo = nullptr;
uint8_t brightness = 255;

constexpr spi_host_device_t kDisplaySpiHost = SPI3_HOST;
constexpr ledc_mode_t kBacklightSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_7;
constexpr ledc_timer_bit_t kBacklightResolution = LEDC_TIMER_8_BIT;
constexpr uint32_t kBacklightFrequencyHz = 256;
constexpr TickType_t kTransferTimeoutTicks = pdMS_TO_TICKS(1000);

bool spiBusInitialized = false;
bool backlightInitialized = false;
SemaphoreHandle_t frameTransferDone = nullptr;

bool IRAM_ATTR OnColorTransferDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* user_ctx)
{
    if (user_ctx == nullptr)
    {
        return false;
    }

    BaseType_t highTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(user_ctx), &highTaskWoken);
    return highTaskWoken == pdTRUE;
}

esp_err_t InitTransferSync()
{
    if (frameTransferDone != nullptr)
    {
        return ESP_OK;
    }

    frameTransferDone = xSemaphoreCreateBinary();
    return frameTransferDone != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ApplyBrightness(uint8_t value)
{
    const esp_err_t dutyStatus = ledc_set_duty(kBacklightSpeedMode, kBacklightChannel, value);
    if (dutyStatus != ESP_OK)
    {
        return dutyStatus;
    }

    return ledc_update_duty(kBacklightSpeedMode, kBacklightChannel);
}

esp_err_t InitBacklight()
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
    timerConfig.clk_cfg = LEDC_AUTO_CLK;

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
    channelConfig.duty = 0;
    channelConfig.hpoint = 0;

    status = ledc_channel_config(&channelConfig);
    if (status != ESP_OK)
    {
        return status;
    }

    backlightInitialized = true;
    return ApplyBrightness(brightness);
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

}  // namespace

bool Init()
{
    esp_err_t status = InitBacklight();
    if (status != ESP_OK)
    {
        return false;
    }

    status = InitTransferSync();
    if (status != ESP_OK)
    {
        return false;
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
        ioConfig.on_color_trans_done = OnColorTransferDone;
        ioConfig.user_ctx = frameTransferDone;

        status = esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kDisplaySpiHost), &ioConfig, &panelIo);
        if (status != ESP_OK)
        {
            return false;
        }
    }

    if (panel == nullptr)
    {
        esp_lcd_panel_dev_config_t panelConfig = {};
        panelConfig.reset_gpio_num = Pins::kDisplayReset;
        panelConfig.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panelConfig.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
        panelConfig.bits_per_pixel = 16;

        status = esp_lcd_new_panel_st7789(panelIo, &panelConfig, &panel);
        if (status != ESP_OK)
        {
            return false;
        }

        status = esp_lcd_panel_reset(panel);
        if (status != ESP_OK)
        {
            return false;
        }

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

        status = esp_lcd_panel_disp_on_off(panel, true);
        if (status != ESP_OK)
        {
            return false;
        }

        uint16_t clearLine[WIDTH] = {};
        for (int row = 0; row < HEIGHT; ++row)
        {
            status = esp_lcd_panel_draw_bitmap(panel, 0, row, WIDTH, row + 1, clearLine);
            if (status != ESP_OK)
            {
                return false;
            }
        }
    }

    return ApplyBrightness(brightness) == ESP_OK;
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

    if (!backlightInitialized)
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
