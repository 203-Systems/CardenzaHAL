#pragma once

// Test-only SDK boundary. The actual HAL sources are linked unchanged; these
// functions inject driver failures and record ownership, without emulating a board.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_STATE = 0x103;
constexpr int ESP_ERR_INVALID_ARG = 0x102, ESP_ERR_NO_MEM = 0x101, ESP_ERR_TIMEOUT = 0x107;
#define IRAM_ATTR
#define DMA_ATTR
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_RETURN_ON_ERROR(call, ...) do { const auto error = (call); if (error != ESP_OK) return error; } while (false)
inline const char* esp_err_to_name(int) { return "injected"; }
// The keyboard registers its scan interrupt on another core through an IPC call: here, at once.
using esp_ipc_func_t = void (*)(void*);
inline esp_err_t esp_ipc_call_blocking(int, esp_ipc_func_t function, void* argument) { function(argument); return ESP_OK; }

namespace Fake {
inline std::string fail;
inline std::int64_t now = 1000000;
inline int mutexes = 0, tasks = 0, timers = 0, taskCreates = 0;
inline int panelCreates = 0, panelInits = 0, draws = 0, drains = 0;
inline int panelResets = 0, panelResetPin = -2, resetReason = 1;
inline bool panelOn = false;
inline int binarySemaphores = 0, callbackRegistrations = 0;
inline int backlightConfigurations = 0, backlightWrites = 0, backlightHoldReleases = 0;
inline unsigned backlightDuty = 0, backlightPendingDuty = 0;
inline std::size_t transferredPixels = 0, backlightRevealedAt = 0;
inline std::array<std::uint16_t, 240 * 135> displayPixels{};
inline std::array<int, 4> lastDraw{};
inline std::vector<std::uint16_t> pendingPixels;
inline bool dmaPending = false;
inline const void* lastPixels = nullptr;
inline int i2cCalls = 0;
inline std::array<std::uint8_t, 256> registers = [] {
  std::array<std::uint8_t, 256> r{};
  r[0xfd] = 0x81; r[0xfe] = 0x55; r[0xff] = 0x01; r[0x0c] = 0x60;
  return r;
}();
inline int mounts = 0, unmounts = 0, statusChecks = 0;
inline bool cardResponds = true;
inline int clockOwner = 0; // 1: speaker, 2: PDM
inline unsigned carrierHz = 0;
inline float carrierDuty = 0;
inline std::uint32_t irFrame = 0;
inline bool Bad(const char* point) { return fail == point; }
inline int Result(const char* point) { return Bad(point) ? ESP_FAIL : ESP_OK; }
}
inline std::int64_t esp_timer_get_time() { return Fake::now; }
constexpr int ESP_RST_POWERON = 1, ESP_RST_SW = 3;
inline int esp_reset_reason() { return Fake::resetReason; }

using BaseType_t = int;
using UBaseType_t = unsigned;
using TickType_t = unsigned;
constexpr int pdFALSE = 0, pdTRUE = 1, pdPASS = 1, pdFAIL = 0;
constexpr unsigned portMAX_DELAY = ~0u;
#define portNUM_PROCESSORS 2
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(ms) (ms)
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
#define portENTER_CRITICAL_ISR(...) ((void)0)
#define portEXIT_CRITICAL_ISR(...) ((void)0)
struct FakeSemaphore { bool binary; int count; };
using SemaphoreHandle_t = FakeSemaphore*;
using TaskHandle_t = int*;
inline SemaphoreHandle_t xSemaphoreCreateMutex() {
  if (Fake::Bad("mutex")) return nullptr;
  ++Fake::mutexes; return new FakeSemaphore{false, 1};
}
inline SemaphoreHandle_t xSemaphoreCreateBinary() {
  if (Fake::Bad("display_semaphore")) return nullptr;
  ++Fake::binarySemaphores; return new FakeSemaphore{true, 0};
}
inline void vSemaphoreDelete(SemaphoreHandle_t h) {
  if (!h) return;
  if (h->binary) --Fake::binarySemaphores; else --Fake::mutexes;
  delete h;
}
inline int xSemaphoreTake(SemaphoreHandle_t h, unsigned) {
  if (!h || (h->binary && h->count == 0)) return pdFALSE;
  if (h->binary) h->count = 0;
  return pdTRUE;
}
inline void xSemaphoreGive(SemaphoreHandle_t h) { if (h) h->count = 1; }
inline int xSemaphoreGiveFromISR(SemaphoreHandle_t h, int* wake) {
  xSemaphoreGive(h); if (wake) *wake = pdFALSE; return pdTRUE;
}
inline int xTaskCreatePinnedToCore(void (*)(void*), const char*, unsigned, void*, unsigned, TaskHandle_t* h, int) {
  ++Fake::taskCreates;
  if (Fake::Bad("task")) { *h = nullptr; return pdFALSE; }
  ++Fake::tasks; *h = new int; return pdPASS;
}
inline int xTaskCreate(void (*fn)(void*), const char* name, unsigned stack, void* arg, unsigned priority, TaskHandle_t* h) {
  return xTaskCreatePinnedToCore(fn, name, stack, arg, priority, h, 0);
}
inline void vTaskDelete(TaskHandle_t h) { if (h) { --Fake::tasks; delete h; } }
inline void vTaskDelay(unsigned ticks) { Fake::now += ticks * 1000; }
inline void xTaskNotifyGive(TaskHandle_t) {}
inline unsigned ulTaskNotifyTake(int, unsigned) { return 1; }
inline void vTaskNotifyGiveFromISR(TaskHandle_t, int*) {}
struct StaticQueue_t {};
using QueueHandle_t = StaticQueue_t*;
inline QueueHandle_t xQueueCreateStatic(unsigned, unsigned, std::uint8_t*, StaticQueue_t* q) { return q; }
inline int xQueueReceive(QueueHandle_t, void*, unsigned) { return pdFALSE; }
inline int xQueueSend(QueueHandle_t, const void*, unsigned) { return pdTRUE; }
inline unsigned uxQueueMessagesWaiting(QueueHandle_t) { return 0; }

using gpio_num_t = int;
constexpr int GPIO_MODE_OUTPUT = 1, GPIO_MODE_INPUT = 2, GPIO_PULLUP_PULLDOWN = 3, GPIO_PULLUP_ONLY = 4;
inline int gpio_reset_pin(int) { return Fake::Result("gpio"); }
inline int gpio_set_direction(int, int) { return Fake::Result("gpio"); }
inline int gpio_set_level(int, int) { return Fake::Result("gpio"); }
inline int gpio_set_pull_mode(int, int) { return Fake::Result("gpio"); }
inline int gpio_hold_dis(int pin) {
  if (pin == 38) ++Fake::backlightHoldReleases;
  return Fake::Result("gpio");
}
struct FakeGpioRegisters { std::uint32_t in = ~0u; };
inline FakeGpioRegisters GPIO;
struct spi_dev_t {};
inline unsigned spi_ll_get_running_cmd(spi_dev_t*) { return 0; }
inline void spi_ll_write_buffer_byte(spi_dev_t*, int, const void*, int) {}
inline void spi_ll_set_mosi_bitlen(spi_dev_t*, int) {}
inline void spi_ll_user_start(spi_dev_t*) {}

struct Timer { bool enabled = false; };
using gptimer_handle_t = Timer*;
struct gptimer_config_t { int clk_src = 0, direction = 0; unsigned resolution_hz = 0; int intr_priority = 0; };
struct gptimer_alarm_event_data_t {};
struct gptimer_event_callbacks_t { bool (*on_alarm)(gptimer_handle_t, const gptimer_alarm_event_data_t*, void*) = nullptr; };
struct gptimer_alarm_config_t { std::uint64_t reload_count = 0, alarm_count = 0; struct { unsigned auto_reload_on_alarm = 0; } flags; };
constexpr int GPTIMER_CLK_SRC_DEFAULT = 0, GPTIMER_COUNT_UP = 0;
inline int gptimer_new_timer(const gptimer_config_t*, gptimer_handle_t* h) {
  if (Fake::Bad("timer_new")) return ESP_FAIL;
  ++Fake::timers; *h = new Timer; return ESP_OK;
}
inline int gptimer_register_event_callbacks(gptimer_handle_t, const gptimer_event_callbacks_t*, void*) { return Fake::Result("timer_callback"); }
inline int gptimer_set_alarm_action(gptimer_handle_t, const gptimer_alarm_config_t*) { return Fake::Result("timer_alarm"); }
inline int gptimer_enable(gptimer_handle_t h) { if (Fake::Bad("timer_enable")) return ESP_FAIL; h->enabled = true; return ESP_OK; }
inline int gptimer_start(gptimer_handle_t) { return Fake::Result("timer_start"); }
inline int gptimer_disable(gptimer_handle_t h) { h->enabled = false; return ESP_OK; }
inline int gptimer_del_timer(gptimer_handle_t h) { --Fake::timers; delete h; return ESP_OK; }
inline int gptimer_get_raw_count(gptimer_handle_t, std::uint64_t* count) { *count = 0; return ESP_OK; }

struct I2sChannel { bool enabled = false, rx = false; int port = 0; };
using i2s_chan_handle_t = I2sChannel*;
using i2s_port_t = int;
using i2s_slot_mode_t = int;
constexpr int I2S_NUM_0 = 0, I2S_NUM_1 = 1, I2S_ROLE_MASTER = 0;
constexpr int I2S_SLOT_MODE_MONO = 1, I2S_SLOT_MODE_STEREO = 2;
constexpr int I2S_DATA_BIT_WIDTH_16BIT = 16, I2S_SLOT_BIT_WIDTH_32BIT = 32, I2S_STD_SLOT_BOTH = 3;
constexpr int I2S_CLK_SRC_DEFAULT = 0, I2S_MCLK_MULTIPLE_256 = 256, I2S_GPIO_UNUSED = -1;
struct i2s_chan_config_t { int id = 0, role = 0; unsigned dma_desc_num = 0, dma_frame_num = 0; bool auto_clear = false; };
#define I2S_CHANNEL_DEFAULT_CONFIG(port, role) i2s_chan_config_t{port, role}
struct i2s_std_slot_config_t { int data_bit_width = 0, slot_mode = 0, slot_bit_width = 0, ws_width = 0, slot_mask = 0; };
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) i2s_std_slot_config_t{bits, mode}
struct i2s_std_gpio_config_t { int mclk = -1, bclk = -1, ws = -1, dout = -1, din = -1; struct { bool mclk_inv = false, bclk_inv = false, ws_inv = false; } invert_flags; };
struct i2s_std_config_t {
  struct { unsigned sample_rate_hz = 0; int clk_src = 0; unsigned ext_clk_freq_hz = 0; int mclk_multiple = 0; } clk_cfg;
  i2s_std_slot_config_t slot_cfg; i2s_std_gpio_config_t gpio_cfg;
};
struct i2s_pdm_rx_gpio_config_t { int clk = -1, din = -1; struct { bool clk_inv = false; } invert_flags; };
struct i2s_pdm_rx_config_t { int clk_cfg = 0, slot_cfg = 0; i2s_pdm_rx_gpio_config_t gpio_cfg; };
#define I2S_PDM_RX_CLK_DEFAULT_CONFIG(rate) (rate)
#define I2S_PDM_RX_SLOT_DEFAULT_CONFIG(bits, mode) (bits)
struct i2s_event_data_t {};
struct i2s_event_callbacks_t { bool (*on_recv_q_ovf)(i2s_chan_handle_t, i2s_event_data_t*, void*) = nullptr; bool (*on_send_q_ovf)(i2s_chan_handle_t, i2s_event_data_t*, void*) = nullptr; };
struct i2s_chan_info_t { unsigned total_dma_buf_size = 3072; };
inline int i2s_new_channel(const i2s_chan_config_t* cfg, i2s_chan_handle_t* tx, i2s_chan_handle_t* rx) {
  if (tx) *tx = new I2sChannel{false, false, cfg->id};
  if (rx) *rx = new I2sChannel{false, true, cfg->id};
  return ESP_OK;
}
inline int i2s_del_channel(i2s_chan_handle_t h) { delete h; return ESP_OK; }
inline int i2s_channel_init_std_mode(i2s_chan_handle_t, const i2s_std_config_t*) { Fake::clockOwner = 1; return ESP_OK; }
inline int i2s_channel_init_pdm_rx_mode(i2s_chan_handle_t, const i2s_pdm_rx_config_t*) { Fake::clockOwner = 2; return Fake::Result("mic_init"); }
inline int i2s_channel_reconfig_std_slot(i2s_chan_handle_t h, const i2s_std_slot_config_t*) { return h->enabled ? ESP_ERR_INVALID_STATE : ESP_OK; }
inline int i2s_channel_reconfig_std_gpio(i2s_chan_handle_t h, const i2s_std_gpio_config_t*) {
  if (h->enabled) return ESP_ERR_INVALID_STATE;
  if (Fake::Bad("speaker_pins")) return ESP_FAIL;
  Fake::clockOwner = 1; return ESP_OK;
}
inline int i2s_channel_reconfig_pdm_rx_gpio(i2s_chan_handle_t h, const i2s_pdm_rx_gpio_config_t*) {
  if (h->enabled) return ESP_ERR_INVALID_STATE;
  Fake::clockOwner = 2; return Fake::Result("mic_pins");
}
inline int i2s_channel_enable(i2s_chan_handle_t h) {
  if (h->enabled) return ESP_ERR_INVALID_STATE;
  if (Fake::Bad(h->rx ? "mic_enable" : "speaker_enable")) return ESP_FAIL;
  h->enabled = true;
  return ESP_OK;
}
inline int i2s_channel_disable(i2s_chan_handle_t h) {
  if (!h->enabled) return ESP_ERR_INVALID_STATE;
  if (Fake::Bad(h->rx ? "mic_disable" : "speaker_disable")) return ESP_FAIL;
  h->enabled = false;
  return ESP_OK;
}
inline int i2s_channel_get_info(i2s_chan_handle_t, i2s_chan_info_t* info) { info->total_dma_buf_size = 3072; return ESP_OK; }
inline int i2s_channel_register_event_callback(i2s_chan_handle_t, const i2s_event_callbacks_t*, void*) { return Fake::Result("mic_callback"); }
inline int i2s_channel_preload_data(i2s_chan_handle_t, const void*, std::size_t bytes, std::size_t* done) { *done = bytes; return ESP_OK; }
inline int i2s_channel_write(i2s_chan_handle_t h, const void*, std::size_t bytes, std::size_t* done, unsigned) { *done = h->enabled ? bytes : 0; return h->enabled ? ESP_OK : ESP_ERR_INVALID_STATE; }
inline int i2s_channel_read(i2s_chan_handle_t, void*, std::size_t, std::size_t* done, unsigned) { *done = 0; return ESP_ERR_TIMEOUT; }

using i2c_master_bus_handle_t = void*;
using i2c_master_dev_handle_t = void*;
constexpr int I2C_ADDR_BIT_LEN_7 = 7;
struct i2c_device_config_t { int dev_addr_length = 0; unsigned device_address = 0, scl_speed_hz = 0, scl_wait_us = 0; };
inline int i2c_master_bus_add_device(void*, const i2c_device_config_t*, void** h) { *h = reinterpret_cast<void*>(1); return ESP_OK; }
inline int i2c_master_transmit(void*, const void* data, std::size_t, int) {
  ++Fake::i2cCalls;
  if (Fake::Bad("i2c")) return ESP_FAIL;
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  Fake::registers[bytes[0]] = bytes[1]; return ESP_OK;
}
inline int i2c_master_transmit_receive(void*, const void* reg, std::size_t, void* value, std::size_t, int) {
  ++Fake::i2cCalls;
  if (Fake::Bad("i2c")) return ESP_FAIL;
  *static_cast<std::uint8_t*>(value) = Fake::registers[*static_cast<const std::uint8_t*>(reg)]; return ESP_OK;
}

using spi_host_device_t = int;
constexpr int SPI3_HOST = 3, SPI_DMA_CH_AUTO = 0;
struct spi_bus_config_t { int mosi_io_num = 0, miso_io_num = 0, sclk_io_num = 0, quadwp_io_num = 0, quadhd_io_num = 0, max_transfer_sz = 0; };
inline int spi_bus_initialize(int, const spi_bus_config_t*, int) { return Fake::Result("spi_bus"); }
using esp_lcd_panel_handle_t = void*;
using esp_lcd_panel_io_handle_t = void*;
using esp_lcd_spi_bus_handle_t = int;
struct esp_lcd_panel_io_spi_config_t { int cs_gpio_num = 0, dc_gpio_num = 0, spi_mode = 0, pclk_hz = 0, trans_queue_depth = 0, lcd_cmd_bits = 0, lcd_param_bits = 0; };
struct esp_lcd_panel_dev_config_t { int reset_gpio_num = 0, rgb_ele_order = 0, data_endian = 0, bits_per_pixel = 0; };
struct esp_lcd_panel_io_event_data_t {};
using esp_lcd_panel_io_color_trans_done_cb_t = bool (*)(void*, esp_lcd_panel_io_event_data_t*, void*);
struct esp_lcd_panel_io_callbacks_t { esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done = nullptr; };
namespace Fake {
inline esp_lcd_panel_io_color_trans_done_cb_t displayCallback = nullptr;
inline void* displayContext = nullptr;
inline void CompleteDisplayTransfer() {
  if (!dmaPending) return;
  dmaPending = false;
  if (displayCallback) displayCallback(reinterpret_cast<void*>(1), nullptr, displayContext);
}
}
constexpr int LCD_RGB_ELEMENT_ORDER_RGB = 0, LCD_RGB_DATA_ENDIAN_LITTLE = 0, LCD_CMD_NOP = 0;
inline int esp_lcd_new_panel_io_spi(int, const esp_lcd_panel_io_spi_config_t*, void** io) { *io = reinterpret_cast<void*>(1); return ESP_OK; }
inline int esp_lcd_new_panel_st7789(void*, const esp_lcd_panel_dev_config_t* config, void** panel) {
  ++Fake::panelCreates; Fake::panelResetPin = config->reset_gpio_num;
  *panel = reinterpret_cast<void*>(2); return ESP_OK;
}
inline int esp_lcd_panel_reset(void*) { ++Fake::panelResets; return Fake::Result("panel_reset"); }
inline int esp_lcd_panel_init(void*) { ++Fake::panelInits; return Fake::Result("panel_init"); }
inline int esp_lcd_panel_invert_color(void*, bool) { return ESP_OK; }
inline int esp_lcd_panel_swap_xy(void*, bool) { return ESP_OK; }
inline int esp_lcd_panel_mirror(void*, bool, bool) { return ESP_OK; }
inline int esp_lcd_panel_set_gap(void*, int, int) { return ESP_OK; }
inline int esp_lcd_panel_disp_on_off(void*, bool on) { Fake::panelOn = on; return ESP_OK; }
inline int esp_lcd_panel_io_register_event_callbacks(void*, const esp_lcd_panel_io_callbacks_t* callbacks, void* context) {
  ++Fake::callbackRegistrations;
  if (Fake::Bad("display_callback")) return ESP_FAIL;
  Fake::displayCallback = callbacks->on_color_trans_done; Fake::displayContext = context;
  return ESP_OK;
}
inline int esp_lcd_panel_draw_bitmap(void*, int x0, int y0, int x1, int y1, const void* pixels) {
  ++Fake::draws;
  if (Fake::Bad("draw")) return ESP_FAIL;
  Fake::lastPixels = pixels; Fake::lastDraw = {x0, y0, x1, y1};
  const auto* words = static_cast<const std::uint16_t*>(pixels);
  Fake::pendingPixels.assign(words, words + (x1 - x0) * (y1 - y0));
  for (int row = y0; row < y1; ++row)
    std::copy_n(words + (row - y0) * (x1 - x0), x1 - x0, Fake::displayPixels.data() + row * 240 + x0);
  Fake::transferredPixels += Fake::pendingPixels.size();
  Fake::dmaPending = true;
  if (!Fake::Bad("display_timeout") && Fake::displayCallback) Fake::CompleteDisplayTransfer();
  return ESP_OK;
}
inline int esp_lcd_panel_io_tx_param(void*, int, const void*, std::size_t) {
  ++Fake::drains; if (Fake::Bad("drain")) return ESP_FAIL;
  Fake::CompleteDisplayTransfer(); return ESP_OK;
}
using ledc_mode_t = int; using ledc_timer_t = int; using ledc_channel_t = int; using ledc_timer_bit_t = int;
constexpr int LEDC_LOW_SPEED_MODE = 0, LEDC_TIMER_0 = 0, LEDC_CHANNEL_7 = 7, LEDC_TIMER_12_BIT = 12;
constexpr int LEDC_USE_APB_CLK = 0, LEDC_INTR_DISABLE = 0;
struct ledc_timer_config_t { int speed_mode = 0, duty_resolution = 0, timer_num = 0; unsigned freq_hz = 0; int clk_cfg = 0; };
struct ledc_channel_config_t { int gpio_num = 0, speed_mode = 0, channel = 0, intr_type = 0, timer_sel = 0, duty = 0, hpoint = 0; };
inline int ledc_timer_config(const ledc_timer_config_t*) { return ESP_OK; }
inline int ledc_channel_config(const ledc_channel_config_t* config) {
  if (config->channel == LEDC_CHANNEL_7) {
    if (Fake::Bad("display_pwm")) return ESP_FAIL;
    ++Fake::backlightConfigurations; Fake::backlightDuty = Fake::backlightPendingDuty = config->duty;
    if (config->duty != 0 && Fake::backlightRevealedAt == 0) Fake::backlightRevealedAt = Fake::transferredPixels;
  }
  return ESP_OK;
}
inline int ledc_set_duty(int, int channel, unsigned duty) {
  if (channel == LEDC_CHANNEL_7) {
    if (Fake::Bad("display_brightness")) return ESP_FAIL;
    ++Fake::backlightWrites; Fake::backlightPendingDuty = duty;
  }
  return ESP_OK;
}
inline int ledc_update_duty(int, int channel) {
  if (channel == LEDC_CHANNEL_7) {
    if (Fake::Bad("display_duty_update")) return ESP_FAIL;
    Fake::backlightDuty = Fake::backlightPendingDuty;
    if (Fake::backlightDuty != 0 && Fake::backlightRevealedAt == 0) Fake::backlightRevealedAt = Fake::transferredPixels;
  }
  return ESP_OK;
}

struct sdmmc_host_t {};
struct sdmmc_slot_config_t { int clk = 0, cmd = 0, d0 = 0, d1 = 0, d2 = 0, d3 = 0, width = 0; };
#define SDMMC_HOST_DEFAULT() sdmmc_host_t{}
#define SDMMC_SLOT_CONFIG_DEFAULT() sdmmc_slot_config_t{}
struct sdmmc_card_t { struct { unsigned capacity = 1024, sector_size = 512; } csd; };
struct esp_vfs_fat_sdmmc_mount_config_t { bool format_if_mount_failed = false; int max_files = 0, allocation_unit_size = 0; };
inline int esp_vfs_fat_sdmmc_mount(const char*, const sdmmc_host_t*, const sdmmc_slot_config_t*, const esp_vfs_fat_sdmmc_mount_config_t*, sdmmc_card_t** card) {
  ++Fake::mounts; if (Fake::Bad("mount")) return ESP_FAIL; *card = new sdmmc_card_t; return ESP_OK;
}
inline int esp_vfs_fat_sdcard_unmount(const char*, sdmmc_card_t* card) { ++Fake::unmounts; delete card; return Fake::Result("unmount"); }
inline int sdmmc_get_status(sdmmc_card_t*) { ++Fake::statusChecks; return Fake::cardResponds ? ESP_OK : ESP_FAIL; }

using rmt_channel_handle_t = int*; using rmt_encoder_handle_t = int*;
constexpr int RMT_CLK_SRC_DEFAULT = 0;
struct rmt_tx_channel_config_t { int gpio_num = 0, clk_src = 0, resolution_hz = 0, mem_block_symbols = 0, trans_queue_depth = 0; };
struct rmt_copy_encoder_config_t {};
struct rmt_transmit_config_t { int loop_count = 0; };
struct rmt_carrier_config_t { unsigned frequency_hz = 0; float duty_cycle = 0; };
struct rmt_symbol_word_t { unsigned level0 = 0, duration0 = 0, level1 = 0, duration1 = 0; };
inline int rmt_new_tx_channel(const rmt_tx_channel_config_t*, int** h) { *h = new int; return ESP_OK; }
inline int rmt_new_copy_encoder(const rmt_copy_encoder_config_t*, int** h) { *h = new int; return ESP_OK; }
inline int rmt_apply_carrier(int*, const rmt_carrier_config_t* cfg) { Fake::carrierHz = cfg->frequency_hz; Fake::carrierDuty = cfg->duty_cycle; return Fake::Result("carrier"); }
inline int rmt_enable(int*) { return ESP_OK; }
inline int rmt_del_channel(int* h) { delete h; return ESP_OK; }
inline int rmt_del_encoder(int* h) { delete h; return ESP_OK; }
inline int rmt_transmit(int*, int*, const void* data, std::size_t, const rmt_transmit_config_t*) {
  const auto* symbols = static_cast<const rmt_symbol_word_t*>(data);
  Fake::irFrame = 0;
  for (unsigned bit = 0; bit < 32; ++bit) if (symbols[bit + 1].duration1 == 1690) Fake::irFrame |= std::uint32_t{1} << bit;
  return ESP_OK;
}
inline int rmt_tx_wait_all_done(int*, unsigned) { return ESP_OK; }
