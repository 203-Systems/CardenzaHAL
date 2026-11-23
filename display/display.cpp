#include "display.h"
#include "changed_regions.h"
#include "scan_order.h"
#include "backlight_brightness.h"
#include "internal/overlay.h"
#include "internal/snapshot.h"
#include "internal/system_brightness.h"
#include "../board/pins.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <optional>
#include <sdkconfig.h>
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
#include <esp_timer.h>
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
// The panel's SPI clock (Kconfig CARDENZA_HAL_DISPLAY_SPI_MHZ). The 203 Launcher runs this panel at 80 MHz on
// every Cardenza (its docs/UX_REVIEW_2026-11-04.md): at 40 MHz a whole frame took about as long as one panel
// scan, so the write ran beside the scan and moving pictures tore in steps.
#ifdef CONFIG_CARDENZA_HAL_DISPLAY_SPI_MHZ
constexpr int kSpiClockMhz = CONFIG_CARDENZA_HAL_DISPLAY_SPI_MHZ;
#else
constexpr int kSpiClockMhz = 80;
#endif
// What goes out is copied into DMA blocks of internal RAM (a framebuffer in PSRAM cannot be sent by DMA), one
// window at a time (scan_order.h). The first block is static; the second (PresentOptions::pipeline) comes from
// the heap at the first frame, so the next window is copied while the panel takes the last one. The system
// menu's overlay paints its bands of eight rows into the first.
constexpr int kTransferBandRows = 8;
constexpr int kBlockPixels = ScanOrder::kBlockPixels;
static_assert(WIDTH * kTransferBandRows <= kBlockPixels);
constexpr uint32_t kTransferTimeoutMs = 20;
DMA_ATTR uint16_t transferBand[kBlockPixels] = {};
uint16_t* secondBlock = nullptr;
bool secondBlockTried = false;
int blockInFlight = 0;  // the block the pending transfer reads (while transferPending)
SemaphoreHandle_t transferDone = nullptr;
bool completionCallbackInstalled = false;
bool transferPending = false;
// display/presenter.h: how PresentChanged works, and what the frames cost.
PresentOptions presentOptions;
FrameStats lastFrame;
PresentTotals totals;
FrameCallback frameCallback = nullptr;
void* frameCallbackContext = nullptr;

// The system menu (system/menu.h) draws over the firmware. Every submission takes displayLock, so the
// menu's task and the firmware's never share the band or the panel.
// - overlayActive: the menu is open: the firmware's frames are not sent at all.
// - captureFrame: the menu keeps a copy of the firmware's latest frame (its picture fades into the menu and
//   back), copied at every Present* while set.
// - overlayDuty: the menu sets the backlight's duty itself (it fades it; 0 to kBacklightFullDuty; -1: the
//   firmware's brightness), and overlayLauncherPwm: at the Launcher's PWM frequency, so that a duty lights the
//   screen as it does in the Launcher (the backlight's driver takes 400 us to rise: the same duty is dimmer in
//   shorter pulses).
bool overlayActive = false;
// The board's screen brightness setting (system/settings.h, internal/system_brightness.h): -1 none (a firmware
// without CardenzaHAL::Init: its own `brightness` as before). firmwareOwns: the firmware set its own level
// since the setting last changed.
int systemPercent = -1;
bool firmwareOwns = false;
// The development channel's screenshot (internal/snapshot.h): the framebuffer last presented, and a frame to
// copy the next one into.
const uint16_t* lastFramebuffer = nullptr;
uint16_t* snapshotFrame = nullptr;
bool snapshotTaken = false;
uint16_t* captureFrame = nullptr;
bool captureValid = false;
int overlayDuty = -1;
bool overlayLauncherPwm = false;
bool launcherPwmApplied = false;
// The 203 Launcher's backlight PWM on the Cardenza (its src/platform/board.cpp).
constexpr uint32_t kLauncherBacklightFrequencyHz = 256;
// The menu's pixels as last sent (ChangedRegions' row segments, made by BeginOverlay and given back by
// EndOverlay; PSRAM when there is some): of each band only what changed is sent again, so a frame of the
// menu's sliding selection bar sends the strips its edges swept. Null (no room): every band whole.
static_assert(kTransferBandRows == ChangedRegions::kBandRows);
uint32_t* overlayHashes = nullptr;
ChangedRegions* overlayRegions = nullptr;
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

// The setting's light: the Launcher's duty curve, at its PWM frequency.
bool SystemShown()
{
    return systemPercent >= 0 && !firmwareOwns;
}

uint16_t FirmwareDuty(uint8_t requested)
{
    return SystemShown() ? static_cast<uint16_t>(detail::LauncherBacklightDuty(systemPercent, detail::kBacklightFullDuty))
                         : detail::BacklightDuty(requested);
}

uint16_t ShownDuty(uint8_t requested)
{
    return overlayDuty >= 0 ? static_cast<uint16_t>(overlayDuty) : FirmwareDuty(requested);
}

void Capture(const uint16_t* framebuffer)
{
    lastFramebuffer = framebuffer;
    if (snapshotFrame != nullptr)
    {
        std::memcpy(snapshotFrame, framebuffer, static_cast<std::size_t>(WIDTH) * HEIGHT * sizeof(uint16_t));
        snapshotFrame = nullptr;
        snapshotTaken = true;
    }
    if (captureFrame == nullptr) return;
    std::memcpy(captureFrame, framebuffer, static_cast<std::size_t>(WIDTH) * HEIGHT * sizeof(uint16_t));
    captureValid = true;
}

esp_err_t ApplyDuty(uint16_t duty)
{
    // The PWM's frequency first: the Launcher's while the system menu asks for it or the system setting is
    // shown, the HAL's otherwise.
    const bool launcherPwm = overlayDuty >= 0 ? overlayLauncherPwm : SystemShown();
    if (backlightInitialized && launcherPwm != launcherPwmApplied &&
        ledc_set_freq(kBacklightSpeedMode, kBacklightTimer,
                      launcherPwm ? kLauncherBacklightFrequencyHz : kBacklightFrequencyHz) == ESP_OK)
        launcherPwmApplied = launcherPwm;
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

esp_err_t InitBacklight(uint16_t initialDuty)
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
    channelConfig.duty = initialDuty;
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
    // ESP-IDF's SPI panel IO ignores this callback's return value (esp_lcd_panel_io_spi.c,
    // lcd_spi_post_trans_color_cb), so the task waiting for the block ran only at the next RTOS tick: a block of
    // well under a millisecond took a whole tick, 10 ms at ESP-IDF's default 100 Hz (a screen took 170 ms). The
    // switch is asked for here, as the 203 Launcher does.
    portYIELD_FROM_ISR(wake);
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
        if (InitBacklight(ShownDuty(brightness)) != ESP_OK) return false;
    }
    if (ApplyDuty(ShownDuty(brightness)) != ESP_OK) return false;
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

TickType_t TransferTimeout()
{
    const TickType_t ticks = pdMS_TO_TICKS(kTransferTimeoutMs);
    return ticks > 0 ? ticks : 1;
}

// The second DMA block while PresentOptions::pipeline asks for it (taken once; given back when it stops).
void UpdateSecondBlock()
{
    if (presentOptions.pipeline)
    {
        if (secondBlockTried) return;
        secondBlockTried = true;
        secondBlock = static_cast<uint16_t*>(
            heap_caps_malloc(kBlockPixels * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return;
    }
    secondBlockTried = false;
    if (secondBlock == nullptr || (transferPending && blockInFlight == 1)) return;
    heap_caps_free(secondBlock);
    secondBlock = nullptr;
}

// One window of the framebuffer (scan_order.h: at most kBlockPixels) to the panel. With two blocks it is copied
// into the one the panel is not reading while the window before is still going out, then sent once that is done.
bool SendWindow(const uint16_t* framebuffer, const Region& window)
{
    int index = 0;
    if (secondBlock != nullptr && transferPending)
        index = 1 - blockInFlight;
    else if (!WaitForTransfer(TransferTimeout()))
        return false;
    uint16_t* block = index == 0 ? transferBand : secondBlock;
    const uint16_t* source = framebuffer + window.y * WIDTH + window.x;
    const std::size_t rowBytes = static_cast<std::size_t>(window.width) * sizeof(uint16_t);
    for (int row = 0; row < window.height; ++row) std::memcpy(block + row * window.width, source + row * WIDTH, rowBytes);
    if (!WaitForTransfer(TransferTimeout())) return false;
    while (xSemaphoreTake(transferDone, 0) == pdTRUE) {}
    if (esp_lcd_panel_draw_bitmap(panel, window.x, window.y, window.x + window.width, window.y + window.height, block) !=
        ESP_OK)
        return false;
    transferPending = true;
    blockInFlight = index;
    return true;
}

uint32_t MicrosSince(int64_t start)
{
    const int64_t elapsed = esp_timer_get_time() - start;
    return elapsed > 0 ? static_cast<uint32_t>(elapsed) : 0;
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
        ioConfig.pclk_hz = kSpiClockMhz * 1000 * 1000;
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

// Sends `regions` (or everything, when the panel needs it) in the panel's scan order and waits until the last
// window is on the panel. Fills in what it sent.
bool PresentLocked(const uint16_t* framebuffer, const Region* regions, std::size_t count, FrameStats& stats)
{
    if (framebuffer == nullptr || (count != 0 && regions == nullptr)) return false;
    stats.regions = static_cast<uint16_t>(std::min<std::size_t>(count, UINT16_MAX));
    Capture(framebuffer);
    // The system menu is on the screen: this frame is not sent, and the first one after it goes whole.
    if (overlayActive)
    {
        fullRefreshRequired = true;
        stats.held = true;
        return true;
    }
    for (std::size_t index = 0; index < count; ++index)
        if (!ValidRegion(regions[index])) return false;
    if (!Init() || !EnsureCompletionCallback()) return false;
    // A timed-out DMA still owns its block. Never overwrite it while the
    // caller continues rendering into its own framebuffer.
    if (!WaitForTransfer(0)) return false;
    UpdateSecondBlock();
    const int64_t start = esp_timer_get_time();
    const Region fullFrame{0, 0, WIDTH, HEIGHT};
    if (fullRefreshRequired)
    {
        regions = &fullFrame;
        count = 1;
        stats.whole = true;
    }
    bool sent = true;
    for (std::size_t first = 0; sent && first < count; first += ScanOrder::kMaxRegions)
    {
        ScanOrder order(regions + first, count - first);
        Region window;
        while (sent && order.Next(window))
        {
            sent = SendWindow(framebuffer, window);
            if (!sent) break;
            ++stats.windows;
            stats.bytes += static_cast<uint32_t>(window.width * window.height) * sizeof(uint16_t);
        }
    }
    // The last window is on the panel before the call returns (and before the light of a first frame comes on).
    if (sent) sent = WaitForTransfer(TransferTimeout());
    stats.sendUs = MicrosSince(start);
    if (!sent || !RevealFrame())
    {
        fullRefreshRequired = true;
        return false;
    }
    fullRefreshRequired = false;
    return true;
}

// What a framebuffer Present* cost, into the totals.
void Account(FrameStats& stats, int64_t start, bool ok)
{
    stats.ok = ok;
    stats.presentUs = MicrosSince(start);
    stats.number = ++totals.frames;
    if (stats.whole) ++totals.wholeFrames;
    if (stats.held) ++totals.heldFrames;
    if (!ok) ++totals.failures;
    totals.bytes += stats.bytes;
    totals.findUs += stats.findUs;
    totals.presentUs += stats.presentUs;
    totals.maxPresentUs = std::max(totals.maxPresentUs, stats.presentUs);
    lastFrame = stats;
}

// After the display's lock is let go: the frame callback.
bool Report(const FrameStats& stats, FrameCallback callback, void* context)
{
    if (callback != nullptr) callback(stats, context);
    return stats.ok;
}

}  // namespace

bool Present(const uint16_t* framebuffer)
{
    const Region fullFrame{0, 0, WIDTH, HEIGHT};
    return Present(framebuffer, &fullFrame, 1);
}

bool Present(const uint16_t* framebuffer, const Region* regions, std::size_t count)
{
    FrameStats stats;
    FrameCallback callback;
    void* context;
    {
        DisplayLock lock;
        const int64_t start = esp_timer_get_time();
        const bool ok = PresentLocked(framebuffer, regions, count, stats);
        Account(stats, start, ok);
        callback = frameCallback;
        context = frameCallbackContext;
    }
    return Report(stats, callback, context);
}

namespace {
// PresentChanged's table of row hashes, made on its first frame (PSRAM when there is some: it is read once a
// frame) and again when the options change. Empty after that: no room, so every frame goes whole.
std::optional<ChangedRegions> changedRegions;
uint32_t* changedHashes = nullptr;
bool changedRegionsTried = false;

void UpdateChangedRegions()
{
    if (changedRegionsTried) return;
    changedRegionsTried = true;
    changedRegions.reset();
    heap_caps_free(changedHashes);
    changedHashes = nullptr;
    const std::size_t bytes = ChangedRegions::HashCount(presentOptions.segmentPixels) * sizeof(uint32_t);
    void* hashes = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (hashes == nullptr) hashes = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (hashes == nullptr) return;
    changedHashes = static_cast<uint32_t*>(hashes);
    changedRegions.emplace(changedHashes, presentOptions.segmentPixels, presentOptions.refreshBands);
}
}  // namespace

bool PresentChanged(const uint16_t* framebuffer)
{
    if (framebuffer == nullptr) return false;
    FrameStats stats;
    FrameCallback callback;
    void* context;
    {
        DisplayLock lock;
        const int64_t start = esp_timer_get_time();
        bool ok = true;
        if (overlayActive)
        {
            Capture(framebuffer);
            fullRefreshRequired = true;
            stats.held = true;
        }
        else
        {
            UpdateChangedRegions();
            const Region fullFrame{0, 0, WIDTH, HEIGHT};
            if (!changedRegions)
                ok = PresentLocked(framebuffer, &fullFrame, 1, stats);
            else
            {
                Region regions[ChangedRegions::kMaxRegions];
                const std::size_t count = changedRegions->Find(framebuffer, regions);
                stats.findUs = MicrosSince(start);
                // A failed send leaves the panel unsure: Present sends the whole screen next time.
                ok = PresentLocked(framebuffer, regions, count, stats);
            }
        }
        Account(stats, start, ok);
        callback = frameCallback;
        context = frameCallbackContext;
    }
    return Report(stats, callback, context);
}

bool SetPresentOptions(const PresentOptions& options)
{
    if (!ChangedRegions::ValidSegment(options.segmentPixels) || options.refreshBands < 0 ||
        options.refreshBands > ChangedRegions::kBands)
        return false;
    DisplayLock lock;
    presentOptions = options;
    changedRegionsTried = false;  // the next PresentChanged makes its table again
    fullRefreshRequired = true;
    return true;
}

PresentOptions GetPresentOptions()
{
    DisplayLock lock;
    return presentOptions;
}

void RequestWholeFrame()
{
    DisplayLock lock;
    fullRefreshRequired = true;
}

FrameStats LastFrame()
{
    DisplayLock lock;
    return lastFrame;
}

PresentTotals Totals()
{
    DisplayLock lock;
    return totals;
}

void ResetTotals()
{
    DisplayLock lock;
    totals = PresentTotals{};
}

void SetFrameCallback(FrameCallback callback, void* context)
{
    DisplayLock lock;
    frameCallback = callback;
    frameCallbackContext = context;
}

uint16_t* AllocateFramebuffer(FrameMemory memory)
{
    constexpr std::size_t bytes = static_cast<std::size_t>(WIDTH) * HEIGHT * sizeof(uint16_t);
    void* frame = nullptr;
    if (memory != FrameMemory::Internal) frame = heap_caps_aligned_alloc(32, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (frame == nullptr && memory != FrameMemory::Psram)
        frame = heap_caps_aligned_alloc(32, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (frame != nullptr) std::memset(frame, 0, bytes);
    return static_cast<uint16_t*>(frame);
}

void FreeFramebuffer(uint16_t* frame)
{
    heap_caps_free(frame);
}

FramePacer::FramePacer(const PacerOptions& options)
    : pacing_(options.ticksPerFrame, options.yieldAfterLate),
      period_(pdMS_TO_TICKS(options.tickMs) > 0 ? pdMS_TO_TICKS(options.tickMs) : 1),
      lastWake_(xTaskGetTickCount())
{
}

bool FramePacer::Wait()
{
    // xTaskDelayUntil keeps the schedule: a tick that ran late does not sleep, and the next ones catch up.
    const bool onTime = xTaskDelayUntil(&lastWake_, period_) == pdTRUE;
    if (pacing_.Ended(onTime))
    {
        vTaskDelay(1);
        lastWake_ = xTaskGetTickCount();
    }
    return onTime;
}

bool DoubleBuffer::Begin(const Options& options)
{
    if (task_ != nullptr) return true;
    frames_[0] = AllocateFramebuffer(options.memory);
    frames_[1] = frames_[0] != nullptr ? AllocateFramebuffer(options.memory) : nullptr;
    handed_ = xSemaphoreCreateBinary();
    idle_ = xSemaphoreCreateBinary();
    const UBaseType_t priority = options.priority != 0 ? options.priority : uxTaskPriorityGet(nullptr);
    if (frames_[1] == nullptr || handed_ == nullptr || idle_ == nullptr ||
        xTaskCreatePinnedToCore(Run, "hal_present", options.stackBytes, this, priority, &task_, options.core) != pdPASS)
    {
        task_ = nullptr;
        End();
        return false;
    }
    back_ = 0;
    front_ = 1;
    result_ = true;
    xSemaphoreGive(idle_);
    return true;
}

void DoubleBuffer::End()
{
    if (task_ != nullptr)
    {
        // The frame being sent finishes first; the task then waits for the next, and goes.
        (void)xSemaphoreTake(idle_, portMAX_DELAY);
        vTaskDelete(task_);
        task_ = nullptr;
    }
    if (handed_ != nullptr) vSemaphoreDelete(handed_);
    if (idle_ != nullptr) vSemaphoreDelete(idle_);
    handed_ = idle_ = nullptr;
    FreeFramebuffer(frames_[0]);
    FreeFramebuffer(frames_[1]);
    frames_[0] = frames_[1] = nullptr;
}

bool DoubleBuffer::Present()
{
    if (task_ == nullptr || xSemaphoreTake(idle_, portMAX_DELAY) != pdTRUE) return false;
    front_ = back_;
    back_ = 1 - back_;
    xSemaphoreGive(handed_);
    return true;
}

bool DoubleBuffer::Wait(TickType_t timeout)
{
    if (task_ == nullptr) return false;
    if (xSemaphoreTake(idle_, timeout) != pdTRUE) return false;
    const bool result = result_;
    xSemaphoreGive(idle_);
    return result;
}

void DoubleBuffer::Run(void* argument)
{
    auto* self = static_cast<DoubleBuffer*>(argument);
    for (;;)
    {
        if (xSemaphoreTake(self->handed_, portMAX_DELAY) != pdTRUE) continue;
        self->result_ = PresentChanged(self->frames_[self->front_]);
        xSemaphoreGive(self->idle_);
    }
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
    firmwareOwns = true;

    if (!firstFramePresented)
    {
        return true;
    }

    return ApplyDuty(ShownDuty(value)) == ESP_OK;
}

uint8_t GetBrightness()
{
    return brightness;
}

}  // namespace CardenzaHAL::Display

namespace CardenzaHAL::Display::detail {

namespace {

// Every row from `painter`, eight rows at a time. `skipUnchanged`: of each band only the rectangle over what
// changed since the overlay last sent it (when the overlay has its table).
bool PaintBands(RegionPainter painter, void* context, bool skipUnchanged)
{
    if (painter == nullptr || !Init() || !EnsureCompletionCallback()) return false;
    const int x = 0, width = WIDTH;
    // A timed-out DMA may still own the band.
    if (!WaitForTransfer(TransferTimeout())) return false;
    const bool track = overlayRegions != nullptr;
    const bool skip = track && skipUnchanged && overlayHashValid;
    overlayHashValid = false;  // until every band has gone out
    for (int y = 0; y < HEIGHT; y += kTransferBandRows)
    {
        const int rows = std::min(kTransferBandRows, HEIGHT - y);
        painter(context, x, y, width, rows, transferBand);
        Region region{x, y, width, rows};
        if (track)
        {
            const Region changed = overlayRegions->FindBand(transferBand, WIDTH, y, y + rows, !skip);
            if (skip)
            {
                if (changed.width == 0) continue;
                // The changed rectangle's rows, packed to the start of the band (each lands at or before
                // where it was).
                region = changed;
                for (int row = 0; row < region.height; ++row)
                    std::memmove(transferBand + row * region.width, transferBand + (region.y - y + row) * WIDTH + region.x,
                                 static_cast<std::size_t>(region.width) * sizeof(uint16_t));
            }
        }
        while (xSemaphoreTake(transferDone, 0) == pdTRUE) {}
        if (esp_lcd_panel_draw_bitmap(panel, region.x, region.y, region.x + region.width, region.y + region.height,
                                      transferBand) != ESP_OK)
            return false;
        transferPending = true;
        blockInFlight = 0;
        if (!WaitForTransfer(TransferTimeout())) return false;
    }
    overlayHashValid = track;
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

void RequestSnapshot(uint16_t* frame)
{
    DisplayLock lock;
    snapshotFrame = frame;
    snapshotTaken = false;
}

bool SnapshotTaken()
{
    DisplayLock lock;
    const bool taken = snapshotTaken;
    snapshotTaken = false;
    return taken;
}

const uint16_t* LastFramebuffer()
{
    DisplayLock lock;
    return lastFramebuffer;
}

bool BeginOverlay()
{
    DisplayLock lock;
    if (!Init() || !EnsureCompletionCallback()) return false;
    overlayActive = true;
    overlayHashValid = false;
    if (overlayRegions == nullptr)
    {
        const std::size_t bytes = ChangedRegions::kHashCount * sizeof(uint32_t);
        overlayHashes = static_cast<uint32_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (overlayHashes == nullptr)
            overlayHashes = static_cast<uint32_t*>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        void* finder = overlayHashes != nullptr
                           ? heap_caps_malloc(sizeof(ChangedRegions), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                           : nullptr;
        if (finder != nullptr) overlayRegions = new (finder) ChangedRegions(overlayHashes);
        else
        {
            heap_caps_free(overlayHashes);
            overlayHashes = nullptr;
        }
    }
    return true;
}

bool PaintOverlay(RegionPainter painter, void* context)
{
    DisplayLock lock;
    if (!overlayActive) return false;
    fullRefreshRequired = true;  // the firmware's next frame goes whole
    if (!PaintBands(painter, context, true)) return false;
    return RevealFrame();
}

void SetBacklightDuty(int duty, bool launcherPwm)
{
    DisplayLock lock;
    overlayDuty = std::clamp(duty, -1, static_cast<int>(kBacklightFullDuty));
    overlayLauncherPwm = launcherPwm;
    if (firstFramePresented) (void)ApplyDuty(ShownDuty(brightness));
}

int FirmwareBacklightDuty()
{
    DisplayLock lock;
    return FirmwareDuty(brightness);
}

void SetSystemBrightness(int percent)
{
    DisplayLock lock;
    systemPercent = std::clamp(percent, 0, 100);
    firmwareOwns = false;
    if (firstFramePresented) (void)ApplyDuty(ShownDuty(brightness));
}

void EndOverlay()
{
    DisplayLock lock;
    overlayActive = false;
    overlayHashValid = false;
    // ChangedRegions holds no resources of its own: its block and the table go back as they are.
    heap_caps_free(overlayRegions);
    heap_caps_free(overlayHashes);
    overlayRegions = nullptr;
    overlayHashes = nullptr;
    fullRefreshRequired = true;
    overlayDuty = -1;
    if (firstFramePresented) (void)ApplyDuty(ShownDuty(brightness));
}

bool SleepPanel()
{
    DisplayLock lock;
    if (!Init()) return false;
    (void)WaitForTransfer(TransferTimeout());
    bool ok = esp_lcd_panel_disp_sleep(panel, true) == ESP_OK;
    // Dark while the chip sleeps: the backlight's LEDC stops low and its pin is held there.
    if (backlightInitialized) ok = ledc_stop(kBacklightSpeedMode, kBacklightChannel, 0) == ESP_OK && ok;
    ok = gpio_set_level(static_cast<gpio_num_t>(Pins::kDisplayBacklight), 0) == ESP_OK && ok;
    ok = gpio_hold_en(static_cast<gpio_num_t>(Pins::kDisplayBacklight)) == ESP_OK && ok;
    return ok;
}

}  // namespace CardenzaHAL::Display::detail
