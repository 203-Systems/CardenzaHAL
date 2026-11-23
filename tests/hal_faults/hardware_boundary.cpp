#include "esp_stub.h"
#include "system/system.h"
#include "keyboard/internal/keyboard_hardware.h"
#include "system/internal/settings_hooks.h"

namespace CardenzaHAL::System {
bool Init() { return !Fake::Bad("system"); }
i2c_master_bus_handle_t GetI2CBus() { return reinterpret_cast<void*>(1); }
bool GetFuncButton() { return false; }
void EnterDeepSleep() {}
namespace detail {
void RestoreHomeButton() { ++Fake::funcButtonRestores; }
}
}
// The board's settings (system/settings.cpp reads the NVS partition, which these fakes do not have): their
// defaults, nothing written.
namespace CardenzaHAL::Settings::detail {
void Load() {}
void ApplyAfterInit() {}
int32_t MicGainQ12Now() { return 4096; }
}
namespace CardenzaHAL::Keyboard::detail {
portMUX_TYPE kKeyboardStateMux = portMUX_INITIALIZER_UNLOCKED;
spi_dev_t hardware;
spi_dev_t* gKeyboardLedSpiHw = &hardware;
void InitKeyboardLedSpi() {}
esp_err_t InitKeyboardLedPwm() { return Fake::Result("pwm"); }
esp_err_t SetKeyboardLedGlobalDuty(uint16_t) { return Fake::Result("pwm"); }
void GateKeyboardLeds(bool) {}
void FastWriteMask(uint64_t, uint64_t) {}
void FastWritePin(int, bool) {}
int FastReadPin(int) { return 1; }
uint64_t KeyboardOutputMask() { return 0; }
bool KeyboardScanTimerCallback(gptimer_handle_t, const gptimer_alarm_event_data_t*, void*) { return false; }
}
