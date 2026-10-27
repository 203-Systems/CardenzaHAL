#pragma once

#include <atomic>
#include <cstring>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "../layout.h"
#include "key_debounce.h"
#include "led_frame.h"

namespace CardenzaHAL::Keyboard {

class KeyboardDriver
{
  private:
    std::vector<Point2D_t> _key_list_buffer;
    uint8_t _last_key_size;

    uint8_t _key_list_scan[8]{};
    uint8_t _task_key_scan[8]{};
    uint8_t _scan_raw_key_scan[8]{};
    detail::KeyDebouncer _debouncer;
    std::atomic<bool> _debounce_pending{false};
    bool _scan_keys_changed = false;
    SemaphoreHandle_t _mutex;
    TaskHandle_t _scan_task_handle;
    volatile uint8_t _scan_row;

    bool _led_enabled;
    bool _scan_led_enabled;
    detail::LedFrameState _led_frame;
    uint8_t _scan_led_col_mask[4][8];
    uint8_t _scan_led_shifted_columns;
    uint8_t _global_brightness;
    uint8_t _pwm_phase;

    void _set_output(uint8_t output);
    uint8_t _get_input();
    void _shift_led_columns(uint8_t value);
    void _commit_led_frame();

    static void _scan_task_entry(void* arg);
    void _scan_task_loop();

  public:
    KeyboardDriver() :
        _last_key_size(0),
        _mutex(nullptr),
        _scan_task_handle(nullptr),
        _scan_row(0),
        _led_enabled(false),
        _scan_led_enabled(false),
        _scan_led_shifted_columns(0xFF),
        _global_brightness(LED_BRIGHTNESS_MAX),
        _pwm_phase(0)
    {
        memset(_scan_led_col_mask, 0, sizeof(_scan_led_col_mask));
    }

    void init();

    Point2D_t getKey();

    uint8_t getKeyNum(Point2D_t keyCoor);

    void updateKeyList();
    std::vector<Point2D_t>& keyList() { return _key_list_buffer; }

    bool isKeyPressing(int keyNum);

    bool isChanged();
    uint8_t isPressed() const { return static_cast<uint8_t>(_key_list_buffer.size()); }

    bool enableLED(bool enabled);
    void setLED(Point2D_t pos, uint8_t brightness);
    void setLEDFrame(const LedFrame& levels);
    void clearAllLEDs();
    void setAllLEDs(uint8_t brightness);
    void setLEDGlobalBrightness(uint8_t level);
    uint8_t ledGlobalBrightness() const { return _global_brightness; }
    bool handleTimerTickFromIsr();
    uint32_t nextTimerTicks() const;
};

}  // namespace CardenzaHAL::Keyboard
