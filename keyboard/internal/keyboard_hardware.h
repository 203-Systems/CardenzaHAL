#pragma once

#include <cstdint>

#include <driver/gptimer.h>
#include <freertos/FreeRTOS.h>
#include <soc/gpio_struct.h>
#include <soc/spi_periph.h>

namespace CardenzaHAL::Keyboard {
class KeyboardDriver;
}

namespace CardenzaHAL::Keyboard::detail {

inline constexpr uint8_t kLedColumnBitOffset = 1;
inline constexpr uint8_t kLedPwmPhaseCount = 4;
// Four weighted bit planes provide exactly 16 electrical duties.
// 128 ticks = 12.8 us, one period of the OE PWM.
inline constexpr uint32_t kLedTimerResolutionHz = 10000000;
inline constexpr uint32_t kLedBitPlaneTicks = 128;
inline constexpr UBaseType_t kScanTaskPriority = 5;
inline constexpr BaseType_t kScanTaskPinnedCore = 0;
// The scan interrupt times the bit planes, and a late one leaves a row lit longer: the shortest plane
// is 12.8 us, so dim keys flicker visibly when it runs tens of microseconds late. USB (its interrupt
// and task) lives on core 0 and kept it waiting hundreds of times a second while a host streamed
// audio (Windows does all the time). On core 1, at the highest C-level priority, it is late about as
// rarely as with USB idle.
inline constexpr BaseType_t kScanInterruptCore = 1;
inline constexpr int kScanInterruptPriority = 3;

extern portMUX_TYPE kKeyboardStateMux;
extern spi_dev_t* gKeyboardLedSpiHw;

constexpr uint64_t PinMask(int pin)
{
    return 1ULL << pin;
}

void FastWriteMask(uint64_t setMask, uint64_t clearMask);
void FastWritePin(int pin, bool level);
int FastReadPin(int pin);
uint64_t KeyboardOutputMask();
void InitKeyboardLedSpi();
esp_err_t InitKeyboardLedPwm();
esp_err_t SetKeyboardLedGlobalDuty(uint16_t duty);
void GateKeyboardLeds(bool active);
bool KeyboardScanTimerCallback(gptimer_handle_t timer,
                               const gptimer_alarm_event_data_t* eventData,
                               void* userContext);

}  // namespace CardenzaHAL::Keyboard::detail
