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
inline constexpr uint32_t kLedRowSelectSettleUs = 8;
inline constexpr uint32_t kLedRowDwellUs = 30;
inline constexpr uint32_t kLedRowPeriodUs = kLedRowSelectSettleUs + kLedRowDwellUs;
inline constexpr UBaseType_t kScanTaskPriority = 5;
inline constexpr BaseType_t kScanTaskPinnedCore = 0;

extern portMUX_TYPE kKeyboardStateMux;
extern spi_dev_t* gKeyboardLedSpiHw;
extern bool gKeyboardLedSpiReady;

constexpr uint64_t PinMask(int pin)
{
    return 1ULL << pin;
}

void FastWriteMask(uint64_t setMask, uint64_t clearMask);
void FastWritePin(int pin, bool level);
int FastReadPin(int pin);
uint64_t KeyboardOutputMask();
void InitKeyboardLedSpi();
bool KeyboardScanTimerCallback(gptimer_handle_t timer,
                               const gptimer_alarm_event_data_t* eventData,
                               void* userContext);

}  // namespace CardenzaHAL::Keyboard::detail
