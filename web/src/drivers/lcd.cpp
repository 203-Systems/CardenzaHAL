// esp_lcd on the web target: the SPI bus, the SPI panel IO and the ST7789 panel driver, ending in the
// display model (st7789.h). The HAL's own display code (display/display.cpp) runs on these unchanged.
//
// Timing is the device's: a colour transfer takes the time its bytes need on the 40 MHz bus (a whole
// 240 x 135 frame about 17 ms, as measured on the board), transfers queue up to trans_queue_depth,
// on_color_trans_done runs in ISR context when one is done, and a parameter transfer waits for the
// queue to drain. The pixels reach the model when queued.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display_state.h"
#include "st7789.h"
#include "../kernel/kernel.h"
#include "../kernel/raw_alloc.h"

namespace k = cardenza_web::kernel;
namespace d = cardenza_web::display;

namespace {

// The board: the display sits on SPI3 with CS 37 and DC 34 (board/pins.h).
constexpr int kDisplayHost = SPI3_HOST;
constexpr int kDisplayCs = 37;
constexpr int kDisplayDc = 34;
// 64,800 bytes took 17 ms on the board at 40 MHz: the bus runs at about 76% of its clock.
constexpr double kBusEfficiency = 12.96 / 17.0;
constexpr uint64_t kTransactionOverheadUs = 12;
constexpr int kMaxQueue = 32;

bool g_busInitialized[SPI_HOST_MAX] = {};

d::St7789* Model()
{
    static d::St7789* model = new (cardenza_web::RawAlloc(sizeof(d::St7789), alignof(d::St7789))) d::St7789();
    return model;
}

struct SpiIo {
    esp_lcd_panel_io_t base;
    bool display;  // wired to the display (else the bytes go nowhere)
    unsigned pclkHz;
    size_t depth;
    esp_lcd_panel_io_color_trans_done_cb_t done;
    void* doneContext;
    uint64_t completions[kMaxQueue];
    int queued;
    uint64_t busFreeUs;
    k::TimedSource source;
    k::WaitQueue waiters;
};

uint64_t TransferUs(const SpiIo* io, size_t bytes)
{
    const double seconds = static_cast<double>(bytes) * 8.0 / (static_cast<double>(io->pclkHz) * kBusEfficiency);
    return kTransactionOverheadUs + static_cast<uint64_t>(seconds * 1e6);
}

void Rearm(SpiIo* io)
{
    k::Reschedule(&io->source, io->queued > 0 ? io->completions[0] : k::kForever);
}

void Complete(k::TimedSource* source, uint64_t now)
{
    SpiIo* io = reinterpret_cast<SpiIo*>(reinterpret_cast<uint8_t*>(source) - offsetof(SpiIo, source));
    while (io->queued > 0 && io->completions[0] <= now)
    {
        std::memmove(io->completions, io->completions + 1, sizeof(uint64_t) * static_cast<size_t>(io->queued - 1));
        --io->queued;
        if (io->done != nullptr)
        {
            esp_lcd_panel_io_event_data_t data{};
            io->done(&io->base, &data, io->doneContext);
        }
        k::WakeAll(&io->waiters);
    }
    Rearm(io);
}

void WaitForRoom(SpiIo* io, int room)
{
    while (io->queued > room)
    {
        if (!k::InTask())
        {
            // No task to block (a call from the scheduler): let the clock run out.
            while (io->queued > room) Complete(&io->source, k::NowUs() > io->completions[0] ? k::NowUs() : io->completions[0]);
            return;
        }
        k::Block(&io->waiters, io->completions[0] + 1);
        if (io->queued > 0 && io->completions[0] <= k::NowUs()) Complete(&io->source, k::NowUs());
    }
}

esp_err_t IoRxParam(esp_lcd_panel_io_t*, int, void*, size_t) { return ESP_ERR_NOT_SUPPORTED; }

esp_err_t IoTxParam(esp_lcd_panel_io_t* base, int command, const void* parameters, size_t length)
{
    auto* io = reinterpret_cast<SpiIo*>(base);
    WaitForRoom(io, 0);  // a parameter transfer is polled: the colour queue drains first
    if (io->display && command >= 0) Model()->Command(static_cast<uint8_t>(command), static_cast<const uint8_t*>(parameters), length);
    k::PreemptionPoint();
    return ESP_OK;
}

esp_err_t IoTxColor(esp_lcd_panel_io_t* base, int command, const void* color, size_t bytes)
{
    auto* io = reinterpret_cast<SpiIo*>(base);
    WaitForRoom(io, static_cast<int>(io->depth) - 1);
    if (io->display)
    {
        if (command >= 0) Model()->Command(static_cast<uint8_t>(command), nullptr, 0);
        Model()->WritePixels(static_cast<const uint8_t*>(color), bytes);
    }
    const uint64_t now = k::NowUs();
    const uint64_t start = io->busFreeUs > now ? io->busFreeUs : now;
    io->busFreeUs = start + TransferUs(io, bytes);
    io->completions[io->queued++] = io->busFreeUs;
    Rearm(io);
    k::PreemptionPoint();
    return ESP_OK;
}

esp_err_t IoDelete(esp_lcd_panel_io_t* base)
{
    auto* io = reinterpret_cast<SpiIo*>(base);
    WaitForRoom(io, 0);
    k::Unregister(&io->source);
    io->~SpiIo();
    std::free(io);
    return ESP_OK;
}

esp_err_t IoRegister(esp_lcd_panel_io_t* base, const esp_lcd_panel_io_callbacks_t* callbacks, void* context)
{
    auto* io = reinterpret_cast<SpiIo*>(base);
    io->done = callbacks != nullptr ? callbacks->on_color_trans_done : nullptr;
    io->doneContext = context;
    return ESP_OK;
}

// ---- the ST7789 panel driver (ESP-IDF's esp_lcd_panel_st7789, on the panel IO) ----------------------

struct Panel {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int resetGpio;
    bool resetActiveHigh;
    int xGap;
    int yGap;
    uint8_t bitsPerPixel;
    uint8_t madctl;
    uint8_t colmod;
    uint8_t ramctl1;
    uint8_t ramctl2;
};

Panel* AsPanel(esp_lcd_panel_t* base) { return reinterpret_cast<Panel*>(base); }

esp_err_t PanelReset(esp_lcd_panel_t* base)
{
    Panel* panel = AsPanel(base);
    if (panel->resetGpio >= 0)
    {
        gpio_set_level(static_cast<gpio_num_t>(panel->resetGpio), panel->resetActiveHigh);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(static_cast<gpio_num_t>(panel->resetGpio), !panel->resetActiveHigh);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    else
    {
        esp_lcd_panel_io_tx_param(panel->io, LCD_CMD_SWRESET, nullptr, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

esp_err_t PanelInit(esp_lcd_panel_t* base)
{
    Panel* panel = AsPanel(base);
    esp_lcd_panel_io_tx_param(panel->io, LCD_CMD_SLPOUT, nullptr, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_lcd_panel_io_tx_param(panel->io, LCD_CMD_MADCTL, &panel->madctl, 1);
    esp_lcd_panel_io_tx_param(panel->io, LCD_CMD_COLMOD, &panel->colmod, 1);
    const uint8_t ramctl[2] = {panel->ramctl1, panel->ramctl2};
    esp_lcd_panel_io_tx_param(panel->io, 0xB0, ramctl, 2);
    return ESP_OK;
}

esp_err_t PanelDelete(esp_lcd_panel_t* base)
{
    std::free(AsPanel(base));
    return ESP_OK;
}

esp_err_t PanelDraw(esp_lcd_panel_t* base, int xStart, int yStart, int xEnd, int yEnd, const void* color)
{
    Panel* panel = AsPanel(base);
    if (xStart >= xEnd || yStart >= yEnd) return ESP_ERR_INVALID_ARG;
    xStart += panel->xGap;
    xEnd += panel->xGap;
    yStart += panel->yGap;
    yEnd += panel->yGap;
    const uint8_t columns[4] = {static_cast<uint8_t>(xStart >> 8), static_cast<uint8_t>(xStart), static_cast<uint8_t>((xEnd - 1) >> 8),
                                static_cast<uint8_t>(xEnd - 1)};
    const uint8_t rows[4] = {static_cast<uint8_t>(yStart >> 8), static_cast<uint8_t>(yStart), static_cast<uint8_t>((yEnd - 1) >> 8),
                             static_cast<uint8_t>(yEnd - 1)};
    esp_lcd_panel_io_tx_param(panel->io, LCD_CMD_CASET, columns, 4);
    esp_lcd_panel_io_tx_param(panel->io, LCD_CMD_RASET, rows, 4);
    const size_t bytes = static_cast<size_t>(xEnd - xStart) * static_cast<size_t>(yEnd - yStart) * panel->bitsPerPixel / 8;
    return esp_lcd_panel_io_tx_color(panel->io, LCD_CMD_RAMWR, color, bytes);
}

esp_err_t PanelInvert(esp_lcd_panel_t* base, bool invert)
{
    return esp_lcd_panel_io_tx_param(AsPanel(base)->io, invert ? LCD_CMD_INVON : LCD_CMD_INVOFF, nullptr, 0);
}

esp_err_t PanelMirror(esp_lcd_panel_t* base, bool mirrorX, bool mirrorY)
{
    Panel* panel = AsPanel(base);
    panel->madctl = static_cast<uint8_t>((panel->madctl & ~(LCD_CMD_MX_BIT | LCD_CMD_MY_BIT)) | (mirrorX ? LCD_CMD_MX_BIT : 0) |
                                         (mirrorY ? LCD_CMD_MY_BIT : 0));
    return esp_lcd_panel_io_tx_param(panel->io, LCD_CMD_MADCTL, &panel->madctl, 1);
}

esp_err_t PanelSwap(esp_lcd_panel_t* base, bool swap)
{
    Panel* panel = AsPanel(base);
    panel->madctl = static_cast<uint8_t>((panel->madctl & ~LCD_CMD_MV_BIT) | (swap ? LCD_CMD_MV_BIT : 0));
    return esp_lcd_panel_io_tx_param(panel->io, LCD_CMD_MADCTL, &panel->madctl, 1);
}

esp_err_t PanelGap(esp_lcd_panel_t* base, int xGap, int yGap)
{
    AsPanel(base)->xGap = xGap;
    AsPanel(base)->yGap = yGap;
    return ESP_OK;
}

esp_err_t PanelOnOff(esp_lcd_panel_t* base, bool on)
{
    return esp_lcd_panel_io_tx_param(AsPanel(base)->io, on ? LCD_CMD_DISPON : LCD_CMD_DISPOFF, nullptr, 0);
}

esp_err_t PanelSleep(esp_lcd_panel_t* base, bool sleep)
{
    esp_lcd_panel_io_tx_param(AsPanel(base)->io, sleep ? LCD_CMD_SLPIN : LCD_CMD_SLPOUT, nullptr, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

}  // namespace

namespace cardenza_web::display {

// The reset line of the display (GPIO33, active low): pulled low, the controller resets.
void ResetLine(bool level)
{
    static bool previous = true;
    if (previous && !level) Model()->Reset();
    previous = level;
}

St7789& Controller() { return *Model(); }

}  // namespace cardenza_web::display

extern "C" {

esp_err_t spi_bus_initialize(spi_host_device_t host_id, const spi_bus_config_t* bus_config, spi_dma_chan_t)
{
    if (host_id <= SPI1_HOST || host_id >= SPI_HOST_MAX || bus_config == nullptr) return ESP_ERR_INVALID_ARG;
    if (g_busInitialized[host_id]) return ESP_ERR_INVALID_STATE;
    g_busInitialized[host_id] = true;
    return ESP_OK;
}

esp_err_t spi_bus_free(spi_host_device_t host_id)
{
    if (host_id <= SPI1_HOST || host_id >= SPI_HOST_MAX || !g_busInitialized[host_id]) return ESP_ERR_INVALID_STATE;
    g_busInitialized[host_id] = false;
    return ESP_OK;
}

}  // extern "C"

// The two constructors are for the HAL's own display code (CARDENZA_WEB_HAL_ONLY): a firmware uses the
// handles the HAL gives.
extern "C" esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus, const esp_lcd_panel_io_spi_config_t* config,
                                                   esp_lcd_panel_io_handle_t* out)
{
    if (config == nullptr || out == nullptr) return ESP_ERR_INVALID_ARG;
    if (bus < 0 || bus >= SPI_HOST_MAX || !g_busInitialized[bus]) return ESP_ERR_INVALID_STATE;
    void* memory = std::calloc(1, sizeof(SpiIo));
    if (memory == nullptr) return ESP_ERR_NO_MEM;
    auto* io = new (memory) SpiIo{};
    io->base.rx_param = IoRxParam;
    io->base.tx_param = IoTxParam;
    io->base.tx_color = IoTxColor;
    io->base.del = IoDelete;
    io->base.register_event_callbacks = IoRegister;
    io->display = bus == kDisplayHost && config->cs_gpio_num == kDisplayCs && config->dc_gpio_num == kDisplayDc;
    io->pclkHz = config->pclk_hz != 0 ? config->pclk_hz : 40000000;
    io->depth = config->trans_queue_depth == 0 ? 1 : (config->trans_queue_depth > kMaxQueue ? kMaxQueue : config->trans_queue_depth);
    io->done = config->on_color_trans_done;
    io->doneContext = config->user_ctx;
    io->source.fire = Complete;
    k::Register(&io->source);
    *out = &io->base;
    return ESP_OK;
}

extern "C" esp_err_t esp_lcd_new_panel_st7789(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t* config,
                                                   esp_lcd_panel_handle_t* out)
{
    if (io == nullptr || config == nullptr || out == nullptr) return ESP_ERR_INVALID_ARG;
    auto* panel = static_cast<Panel*>(std::calloc(1, sizeof(Panel)));
    if (panel == nullptr) return ESP_ERR_NO_MEM;
    panel->io = io;
    panel->resetGpio = config->reset_gpio_num;
    panel->resetActiveHigh = config->flags.reset_active_high;
    panel->bitsPerPixel = static_cast<uint8_t>(config->bits_per_pixel);
    panel->madctl = config->rgb_ele_order == LCD_RGB_ELEMENT_ORDER_BGR ? LCD_CMD_BGR_BIT : 0;
    panel->colmod = config->bits_per_pixel == 18 ? 0x66 : 0x55;
    panel->ramctl1 = 0x00;
    panel->ramctl2 = static_cast<uint8_t>(0xF0 | (config->data_endian == LCD_RGB_DATA_ENDIAN_LITTLE ? 0x08 : 0));
    if (panel->resetGpio >= 0)
    {
        gpio_set_direction(static_cast<gpio_num_t>(panel->resetGpio), GPIO_MODE_OUTPUT);
    }
    panel->base.reset = PanelReset;
    panel->base.init = PanelInit;
    panel->base.del = PanelDelete;
    panel->base.draw_bitmap = PanelDraw;
    panel->base.invert_color = PanelInvert;
    panel->base.mirror = PanelMirror;
    panel->base.swap_xy = PanelSwap;
    panel->base.set_gap = PanelGap;
    panel->base.disp_on_off = PanelOnOff;
    panel->base.disp_sleep = PanelSleep;
    *out = &panel->base;
    return ESP_OK;
}

extern "C" {

esp_err_t esp_lcd_panel_io_rx_param(esp_lcd_panel_io_handle_t io, int lcd_cmd, void* param, size_t param_size)
{
    return io == nullptr ? ESP_ERR_INVALID_ARG : io->rx_param(io, lcd_cmd, param, param_size);
}

esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int lcd_cmd, const void* param, size_t param_size)
{
    return io == nullptr ? ESP_ERR_INVALID_ARG : io->tx_param(io, lcd_cmd, param, param_size);
}

esp_err_t esp_lcd_panel_io_tx_color(esp_lcd_panel_io_handle_t io, int lcd_cmd, const void* color, size_t color_size)
{
    return io == nullptr ? ESP_ERR_INVALID_ARG : io->tx_color(io, lcd_cmd, color, color_size);
}

esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io) { return io == nullptr ? ESP_ERR_INVALID_ARG : io->del(io); }

esp_err_t esp_lcd_panel_io_register_event_callbacks(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_io_callbacks_t* cbs, void* user_ctx)
{
    return io == nullptr ? ESP_ERR_INVALID_ARG : io->register_event_callbacks(io, cbs, user_ctx);
}

esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t panel) { return panel == nullptr ? ESP_ERR_INVALID_ARG : panel->reset(panel); }
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t panel) { return panel == nullptr ? ESP_ERR_INVALID_ARG : panel->init(panel); }
esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t panel) { return panel == nullptr ? ESP_ERR_INVALID_ARG : panel->del(panel); }

esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x_start, int y_start, int x_end, int y_end, const void* color_data)
{
    return panel == nullptr ? ESP_ERR_INVALID_ARG : panel->draw_bitmap(panel, x_start, y_start, x_end, y_end, color_data);
}

esp_err_t esp_lcd_panel_mirror(esp_lcd_panel_handle_t panel, bool mirror_x, bool mirror_y)
{
    return panel == nullptr || panel->mirror == nullptr ? ESP_ERR_NOT_SUPPORTED : panel->mirror(panel, mirror_x, mirror_y);
}

esp_err_t esp_lcd_panel_swap_xy(esp_lcd_panel_handle_t panel, bool swap_axes)
{
    return panel == nullptr || panel->swap_xy == nullptr ? ESP_ERR_NOT_SUPPORTED : panel->swap_xy(panel, swap_axes);
}

esp_err_t esp_lcd_panel_set_gap(esp_lcd_panel_handle_t panel, int x_gap, int y_gap)
{
    return panel == nullptr || panel->set_gap == nullptr ? ESP_ERR_NOT_SUPPORTED : panel->set_gap(panel, x_gap, y_gap);
}

esp_err_t esp_lcd_panel_invert_color(esp_lcd_panel_handle_t panel, bool invert_color_data)
{
    return panel == nullptr || panel->invert_color == nullptr ? ESP_ERR_NOT_SUPPORTED : panel->invert_color(panel, invert_color_data);
}

esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t panel, bool on_off)
{
    return panel == nullptr || panel->disp_on_off == nullptr ? ESP_ERR_NOT_SUPPORTED : panel->disp_on_off(panel, on_off);
}

esp_err_t esp_lcd_panel_disp_off(esp_lcd_panel_handle_t panel, bool off) { return esp_lcd_panel_disp_on_off(panel, !off); }

esp_err_t esp_lcd_panel_disp_sleep(esp_lcd_panel_handle_t panel, bool sleep)
{
    return panel == nullptr || panel->disp_sleep == nullptr ? ESP_ERR_NOT_SUPPORTED : panel->disp_sleep(panel, sleep);
}

}  // extern "C"
