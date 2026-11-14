// CardenzaHAL::Keyboard on the web target. The device scans a 4 x 14 matrix from a timer interrupt
// and drives the key LEDs in four bit planes; here the page's keys are the matrix. What the firmware
// sees is made by the HAL's own code: the same debouncer and four-key guard (internal/key_debounce.h,
// internal/key_count_guard.h) in a "kb_scan" task like the device's (priority 5, core 0), the same
// key list order, and the LEDs read back from the same scan masks (internal/led_frame.h).

#include "keyboard/keyboard.h"

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "keyboard/internal/key_debounce.h"
#include "keyboard/internal/led_frame.h"
#include "keyboard/layout.h"

#include "web/src/kernel/kernel.h"
#include "web_keyboard.h"

namespace CardenzaHAL::Keyboard {

namespace {

namespace k = cardenza_web::kernel;

constexpr UBaseType_t kScanTaskPriority = 5;
constexpr BaseType_t kScanTaskCore = 0;

uint8_t g_raw[8] = {};            // what the page holds, as the matrix rows read
uint8_t g_published[8] = {};      // the debounced rows (under g_mutex)
uint8_t g_listRows[8] = {};       // the rows keyList() was made from
std::vector<Point2D_t> g_list;
uint8_t g_lastCount = 0;
detail::KeyDebouncer g_debouncer;
SemaphoreHandle_t g_mutex = nullptr;
TaskHandle_t g_scanTask = nullptr;
bool g_initialized = false;

detail::LedFrameState g_leds;
bool g_ledEnabled = false;
uint8_t g_global = LED_BRIGHTNESS_MAX;
cardenza_web::keyboard::LedSnapshot g_snapshot{};

void Publish()
{
    // What the LED scan would show: the levels back from the bit planes the device shifts out.
    const auto masks = g_leds.BuildScanMasks();
    for (std::size_t row = 0; row < 8; ++row)
    {
        const std::size_t y = row < 4 ? 3 - row : 7 - row;
        const std::size_t xBase = row < 4 ? 1 : 0;
        for (std::size_t column = 0; column < 7; ++column)
        {
            uint8_t level = 0;
            for (std::size_t phase = 0; phase < masks.size(); ++phase)
                if (masks[phase][row] & (1u << (column + 1))) level |= static_cast<uint8_t>(1u << phase);
            g_snapshot.levels[y * LED_COLUMNS + xBase + column * 2] = g_ledEnabled ? level : 0;
        }
    }
    g_snapshot.global = g_global;
    g_snapshot.enabled = g_ledEnabled ? 1 : 0;
    ++g_snapshot.serial;
}

void ScanTask(void*)
{
    for (;;)
    {
        // Woken by a key the page changed; while a change is settling, every millisecond.
        ulTaskNotifyTake(pdTRUE, g_debouncer.Pending() ? pdMS_TO_TICKS(1) : portMAX_DELAY);
        uint8_t raw[8];
        std::memcpy(raw, g_raw, sizeof(raw));
        if (!g_debouncer.Update(raw, static_cast<uint32_t>(esp_timer_get_time()))) continue;
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        std::memcpy(g_published, g_debouncer.Rows().data(), sizeof(g_published));
        xSemaphoreGive(g_mutex);
    }
}

void WakeScan(void*)
{
    if (g_scanTask == nullptr) return;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(g_scanTask, &woken);
}

}  // namespace

bool init()
{
    if (g_initialized) return true;
    g_mutex = xSemaphoreCreateMutex();
    if (g_mutex == nullptr) return false;
    g_list.reserve(LED_ROWS * LED_COLUMNS);
    if (xTaskCreatePinnedToCore(ScanTask, "kb_scan", 4096, nullptr, kScanTaskPriority, &g_scanTask, kScanTaskCore) != pdPASS)
    {
        vSemaphoreDelete(g_mutex);
        g_mutex = nullptr;
        return false;
    }
    Publish();
    g_initialized = true;
    return true;
}

Point2D_t getKey()
{
    updateKeyList();
    if (g_list.empty()) return {-1, -1};
    return g_list[0];
}

uint8_t getKeyNum(Point2D_t keyCoor)
{
    if (keyCoor.x < 0 || keyCoor.y < 0) return 0;
    return static_cast<uint8_t>(keyCoor.y * 14 + keyCoor.x + 1);
}

void updateKeyList()
{
    k::PreemptionPoint();
    if (g_mutex == nullptr) return;
    uint8_t snapshot[8];
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    std::memcpy(snapshot, g_published, sizeof(snapshot));
    xSemaphoreGive(g_mutex);
    if (std::memcmp(snapshot, g_listRows, sizeof(snapshot)) == 0) return;
    std::memcpy(g_listRows, snapshot, sizeof(snapshot));
    g_list.clear();
    for (int row = 0; row < 8; ++row)
        for (int column = 0; column < 7; ++column)
        {
            if (!(snapshot[row] & (1u << column))) continue;
            g_list.push_back({row > 3 ? X_map_chart[column].x_1 : X_map_chart[column].x_2, 3 - (row > 3 ? row - 4 : row)});
        }
}

std::vector<Point2D_t>& keyList() { return g_list; }

bool isKeyPressing(int keyNum)
{
    for (const auto& key : g_list)
        if (getKeyNum(key) == keyNum) return true;
    return false;
}

bool isChanged()
{
    if (g_lastCount != g_list.size())
    {
        g_lastCount = static_cast<uint8_t>(g_list.size());
        return true;
    }
    return false;
}

uint8_t isPressed() { return static_cast<uint8_t>(g_list.size()); }

bool enableLED(bool enabled)
{
    if (enabled == g_ledEnabled) return g_ledEnabled;
    g_ledEnabled = enabled;
    Publish();
    return g_ledEnabled;
}

void setLED(Point2D_t pos, uint8_t brightness)
{
    if (g_leds.SetKey(pos, brightness)) Publish();
}

void setLEDFrame(const LedFrame& levels)
{
    if (g_leds.Assign(levels)) Publish();
}

void clearAllLEDs()
{
    if (g_leds.Fill(0)) Publish();
}

void setAllLEDs(uint8_t brightness)
{
    if (g_leds.Fill(brightness)) Publish();
}

void setLEDGlobalBrightness(uint8_t level)
{
    g_global = level > LED_BRIGHTNESS_MAX ? LED_BRIGHTNESS_MAX : level;
    Publish();
}

uint8_t ledGlobalBrightness() { return g_global; }

}  // namespace CardenzaHAL::Keyboard

namespace cardenza_web::keyboard {

void SetKey(int x, int y, bool down)
{
    if (x < 0 || x >= 14 || y < 0 || y >= 4) return;
    // The matrix: odd columns on rows 0..3, even columns on rows 4..7 (the HAL's X_map_chart).
    const int row = (x & 1) ? 3 - y : 7 - y;
    const int bit = (x & 1) ? (x - 1) / 2 : x / 2;
    const uint8_t before = CardenzaHAL::Keyboard::g_raw[row];
    if (down) CardenzaHAL::Keyboard::g_raw[row] |= static_cast<uint8_t>(1u << bit);
    else CardenzaHAL::Keyboard::g_raw[row] &= static_cast<uint8_t>(~(1u << bit));
    if (before != CardenzaHAL::Keyboard::g_raw[row]) kernel::RunIsr(CardenzaHAL::Keyboard::WakeScan, nullptr);
}

void ReleaseAll()
{
    std::memset(CardenzaHAL::Keyboard::g_raw, 0, sizeof(CardenzaHAL::Keyboard::g_raw));
    kernel::RunIsr(CardenzaHAL::Keyboard::WakeScan, nullptr);
}

const LedSnapshot& Leds() { return CardenzaHAL::Keyboard::g_snapshot; }

}  // namespace cardenza_web::keyboard
