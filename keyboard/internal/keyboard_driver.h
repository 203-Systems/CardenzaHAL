#pragma once

#include <cstring>
#include <vector>

#include <driver/gptimer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "../layout.h"

namespace CardenzaHAL::Keyboard {

class KeyboardDriver
{
  private:
    std::vector<Point2D_t> _key_list_buffer;
    uint8_t _last_key_size;

    std::vector<Point2D_t> _task_key_list;
    uint8_t _raw_key_scan[8];
    uint8_t _debounced_key_scan[8];
    uint8_t _candidate_key_scan[8];
    uint32_t _debounce_since_us[8];
    uint8_t _scan_raw_key_scan[8];
    SemaphoreHandle_t _mutex;
    TaskHandle_t _scan_task_handle;
    gptimer_handle_t _scan_timer;
    bool _scan_running;
    volatile uint8_t _scan_row;
    volatile uint8_t _diag_scan_row;

    bool _led_enabled;
    bool _scan_led_enabled;
    uint8_t _led_level[4][14];
    uint8_t _scan_led_level[4][14];
    uint8_t _scan_led_col_mask[4][8];
    uint8_t _scan_led_shifted_columns;
    uint8_t _global_brightness;
    bool _global_perceptual_brightness;
    uint8_t _pwm_phase;

    volatile bool _diag_direct_mode;
    volatile int _diag_en_level;
    volatile uint8_t _diag_col;
    volatile uint8_t _diag_row;

    void _set_output(uint8_t output);
    uint8_t _get_input();
    void _shift_led_columns(uint8_t value);
    uint8_t _compute_led_col_mask(uint8_t scan_index, uint8_t phase) const;
    void _commit_led_frame();

    static void _scan_task_entry(void* arg);
    void _scan_task_loop();

  public:
    KeyboardDriver() :
        _last_key_size(0),
        _mutex(nullptr),
        _scan_task_handle(nullptr),
        _scan_timer(nullptr),
        _scan_running(false),
        _scan_row(0),
        _diag_scan_row(0),
        _led_enabled(false),
        _scan_led_enabled(false),
        _scan_led_shifted_columns(0xFF),
        _global_brightness(LED_BRIGHTNESS_MAX),
        _global_perceptual_brightness(true),
        _pwm_phase(0),
        _diag_direct_mode(false),
        _diag_en_level(1),
        _diag_col(0),
        _diag_row(0)
    {
        memset(_raw_key_scan, 0, sizeof(_raw_key_scan));
        memset(_debounced_key_scan, 0, sizeof(_debounced_key_scan));
        memset(_candidate_key_scan, 0, sizeof(_candidate_key_scan));
        memset(_debounce_since_us, 0, sizeof(_debounce_since_us));
        memset(_scan_raw_key_scan, 0, sizeof(_scan_raw_key_scan));
        memset(_led_level, 0, sizeof(_led_level));
        memset(_scan_led_level, 0, sizeof(_scan_led_level));
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
    void clearAllLEDs();
    void setAllLEDs(uint8_t brightness);
    void setLEDGlobalBrightness(uint8_t level);
    void setLEDGlobalPerceptualBrightness(bool enabled);
    bool ledGlobalPerceptualBrightness() const { return _global_perceptual_brightness; }
    uint8_t ledGlobalBrightness() const { return _global_brightness; }
    void updateLED() {}
    bool handleTimerTickFromIsr();
    uint32_t nextTimerTicks() const;

    void suspendScan();
    void resumeScan();
    void setDiagDirect(bool enable, int en_level = 1, uint8_t col = 0, uint8_t row = 0);
};

}  // namespace CardenzaHAL::Keyboard
