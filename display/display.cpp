#include "display.h"
#include "changed_regions.h"
#include "backlight_brightness.h"
#include "internal/overlay.h"
#include "../board/pins.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
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

// The system menu (system/menu.h) draws over the firmware. Every submission takes displayLock, so the
// menu's task and the firmware's never share the band or the panel.
// - curtainEdge: while HOME is held the menu comes out from the left: the firmware's frames are sent only
//   right of it.
// - overlayActive: the menu is open: the firmware's frames are not sent at all.
// - captureFrame: the menu keeps a copy of the firmware's latest frame (its picture fades into the menu and
//   back), copied at every Present* while set.
// - overlayBacklight: the menu sets the backlight (it fades it); the firmware's brightness comes back after.
bool overlayActive = false;
int curtainEdge = 0;
uint16_t* captureFrame = nullptr;
bool captureValid = false;
bool overlayBacklight = false;
uint8_t overlayBacklightValue = 0;
// The menu's bands as last sent (FNV-1a): a band that did not change is not sent again.
constexpr int kOverlayBands = (HEIGHT + kTransferBandRows - 1) / kTransferBandRows;
uint32_t overlayBandHash[kOverlayBands] = {};
bool overlayHashValid = false;
SemaphoreHandle_t displayLock = nullptr;
portMUX_TYPE displayLockMux = portMUX_INITIALIZER_UNLOCKED;

class DisplayLock {
  public:
    DisplayLock()
    {
        if (displayLock == nullptr)
        {
            SemaphoreHandle_t made = xSemaphoreCreateMutex();
            portENTER_CRITICAL(&displayLockMux);
            if (displayLock == nullptr)
            {
                displayLock = made;
                made = nullptr;
            }
            portEXIT_CRITICAL(&displayLockMux);
            if (made != nullptr) vSemaphoreDelete(made);
        }
        held_ = displayLock != nullptr && xSemaphoreTake(displayLock, portMAX_DELAY) == pdTRUE;
    }
    ~DisplayLock()
    {
        if (held_) xSemaphoreGive(displayLock);
    }
    DisplayLock(const DisplayLock&) = delete;
    DisplayLock& operator=(const DisplayLock&) = delete;

  private:
    bool held_ = false;
};

uint8_t ShownBrightness(uint8_t requested)
{
    return overlayBacklight ? overlayBacklightValue : requested;
}

void Capture(const uint16_t* framebuffer)
{
    if (captureFrame == nullptr) return;
    std::memcpy(captureFrame, framebuffer, static_cast<std::size_t>(WIDTH) * HEIGHT * sizeof(uint16_t));
    captureValid = true;
}

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
        if (InitBacklight(ShownBrightness(brightness)) != ESP_OK) return false;
    }
    if (ApplyBrightness(ShownBrightness(brightness)) != ESP_OK) return false;
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

namespace {

bool PresentLocked(const uint16_t* framebuffer, const Region* regions, std::size_t count)
{
    if (framebuffer == nullptr || (count != 0 && regions == nullptr)) return false;
    Capture(framebuffer);
    // The system menu is on the screen: this frame is not sent, and the first one after it goes whole.
    if (overlayActive)
    {
        fullRefreshRequired = true;
        return true;
    }
    for (std::size_t index = 0; index < count; ++index)
        if (!ValidRegion(regions[index])) return false;
    if (!Init() || !EnsureCompletionCallback()) return false;
    // A timed-out DMA still owns transferBand. Never overwrite it while the
    // caller continues rendering into its own framebuffer.
    if (!WaitForTransfer(0)) return false;
    // Left of the curtain's edge the system menu is coming out: only the rest is the firmware's.
    const auto send = [&](Region region) {
        if (curtainEdge > 0)
        {
            const int right = region.x + region.width;
            region.x = std::max(region.x, curtainEdge);
            region.width = right - region.x;
            if (region.width <= 0) return true;
        }
        return SendRegion(framebuffer, region);
    };
    bool sent = true;
    if (fullRefreshRequired)
        sent = send(Region{0, 0, WIDTH, HEIGHT});
    else
        for (std::size_t index = 0; sent && index < count; ++index)
            sent = send(regions[index]);
    if (!sent || !RevealFrame())
    {
        fullRefreshRequired = true;
        return false;
    }
    fullRefreshRequired = false;
    return true;
}

}  // namespace

bool Present(const uint16_t* framebuffer)
{
    const Region fullFrame{0, 0, WIDTH, HEIGHT};
    return Present(framebuffer, &fullFrame, 1);
}

bool Present(const uint16_t* framebuffer, const Region* regions, std::size_t count)
{
    DisplayLock lock;
    return PresentLocked(framebuffer, regions, count);
}

namespace {
// PresentChanged's table of row hashes, made on its first frame (PSRAM when there is some: it is read
// once a frame). Null after that: no room, so every frame goes whole.
ChangedRegions* changedRegions = nullptr;
bool changedRegionsTried = false;
}

bool PresentChanged(const uint16_t* framebuffer)
{
    if (framebuffer == nullptr) return false;
    DisplayLock lock;
    if (overlayActive)
    {
        Capture(framebuffer);
        fullRefreshRequired = true;
        return true;
    }
    if (!changedRegionsTried)
    {
        changedRegionsTried = true;
        const std::size_t bytes = ChangedRegions::kHashCount * sizeof(uint32_t);
        void* hashes = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (hashes == nullptr) hashes = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        void* finder = hashes != nullptr ? heap_caps_malloc(sizeof(ChangedRegions), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : nullptr;
        if (finder != nullptr) changedRegions = new (finder) ChangedRegions(static_cast<uint32_t*>(hashes));
        else if (hashes != nullptr) heap_caps_free(hashes);
    }
    const Region fullFrame{0, 0, WIDTH, HEIGHT};
    if (changedRegions == nullptr) return PresentLocked(framebuffer, &fullFrame, 1);
    Region regions[ChangedRegions::kMaxRegions];
    const std::size_t count = changedRegions->Find(framebuffer, regions);
    // A failed send leaves the panel unsure: Present sends the whole screen next time.
    return PresentLocked(framebuffer, regions, count);
}

bool Present()
{
    DisplayLock lock;
    if (overlayActive) return true;
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
    DisplayLock lock;
    brightness = value;

    if (!firstFramePresented)
    {
        return true;
    }

    return ApplyBrightness(ShownBrightness(value)) == ESP_OK;
}

uint8_t GetBrightness()
{
    return brightness;
}

}  // namespace CardenzaHAL::Display

namespace CardenzaHAL::Display::detail {

namespace {

uint32_t BandHash(const uint16_t* pixels, std::size_t count)
{
    uint32_t hash = 2166136261u;
    const auto* bytes = reinterpret_cast<const uint8_t*>(pixels);
    for (std::size_t i = 0; i < count * sizeof(uint16_t); ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

// Columns [x, x + width) of every row from `painter`, eight rows at a time; `skipUnchanged`: bands as the
// overlay last sent them are not sent again (whole-width bands only).
bool PaintBands(RegionPainter painter, void* context, int x, int width, bool skipUnchanged)
{
    if (painter == nullptr || !Init() || !EnsureCompletionCallback()) return false;
    x = std::clamp(x, 0, WIDTH);
    width = std::clamp(width, 0, WIDTH - x);
    if (width == 0) return true;
    // A timed-out DMA may still own the band.
    if (!WaitForTransfer(pdMS_TO_TICKS(kTransferTimeoutMs))) return false;
    const bool whole = x == 0 && width == WIDTH;
    for (int band = 0, y = 0; y < HEIGHT; ++band, y += kTransferBandRows)
    {
        const int rows = std::min(kTransferBandRows, HEIGHT - y);
        painter(context, x, y, width, rows, transferBand);
        if (whole)
        {
            const uint32_t hash = BandHash(transferBand, static_cast<std::size_t>(width) * rows);
            if (skipUnchanged && overlayHashValid && overlayBandHash[band] == hash) continue;
            overlayBandHash[band] = hash;
        }
        while (xSemaphoreTake(transferDone, 0) == pdTRUE) {}
        if (esp_lcd_panel_draw_bitmap(panel, x, y, x + width, y + rows, transferBand) != ESP_OK) return false;
        transferPending = true;
        if (!WaitForTransfer(pdMS_TO_TICKS(kTransferTimeoutMs))) return false;
    }
    overlayHashValid = whole;
    return true;
}

}  // namespace

bool Ready()
{
    DisplayLock lock;
    return displayInitialized && firstFramePresented;
}

void SetCapture(uint16_t* frame)
{
    DisplayLock lock;
    captureFrame = frame;
    captureValid = false;
}

bool CaptureValid()
{
    DisplayLock lock;
    return captureFrame != nullptr && captureValid;
}

void SetCurtain(int edge, bool refresh)
{
    DisplayLock lock;
    edge = std::clamp(edge, 0, WIDTH);
    // Columns the curtain gives back and the menu did not repaint: the firmware's next frame goes whole.
    if (refresh && edge < curtainEdge) fullRefreshRequired = true;
    curtainEdge = edge;
}

bool PaintColumns(RegionPainter painter, void* context, int x, int width)
{
    DisplayLock lock;
    return PaintBands(painter, context, x, width, false);
}

bool BeginOverlay()
{
    DisplayLock lock;
    if (!Init() || !EnsureCompletionCallback()) return false;
    overlayActive = true;
    overlayHashValid = false;
    return true;
}

bool PaintOverlay(RegionPainter painter, void* context)
{
    DisplayLock lock;
    if (!overlayActive) return false;
    fullRefreshRequired = true;  // the firmware's next frame goes whole
    if (!PaintBands(painter, context, 0, WIDTH, true)) return false;
    return RevealFrame();
}

void SetBacklight(int value)
{
    DisplayLock lock;
    overlayBacklight = value >= 0;
    overlayBacklightValue = static_cast<uint8_t>(std::clamp(value, 0, 255));
    if (firstFramePresented) (void)ApplyBrightness(ShownBrightness(brightness));
}

void EndOverlay()
{
    DisplayLock lock;
    overlayActive = false;
    overlayHashValid = false;
    curtainEdge = 0;
    fullRefreshRequired = true;
    overlayBacklight = false;
    if (firstFramePresented) (void)ApplyBrightness(brightness);
}

bool SleepPanel()
{
    DisplayLock lock;
    if (!Init()) return false;
    (void)WaitForTransfer(pdMS_TO_TICKS(kTransferTimeoutMs));
    bool ok = esp_lcd_panel_disp_sleep(panel, true) == ESP_OK;
    // Dark while the chip sleeps: the backlight's LEDC stops low and its pin is held there.
    if (backlightInitialized) ok = ledc_stop(kBacklightSpeedMode, kBacklightChannel, 0) == ESP_OK && ok;
    ok = gpio_set_level(static_cast<gpio_num_t>(Pins::kDisplayBacklight), 0) == ESP_OK && ok;
    ok = gpio_hold_en(static_cast<gpio_num_t>(Pins::kDisplayBacklight)) == ESP_OK && ok;
    return ok;
}

}  // namespace CardenzaHAL::Display::detail
