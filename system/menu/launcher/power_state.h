// Copied from the 203 Launcher (https://github.com/203-Systems/203-launcher, MIT; LICENSE and
// NOTICE.md there) by tools/launcher_ui.py: src/platform/power_state.h at 4409843d25be.
// Do not edit: change it in the Launcher and copy it again.
#pragma once

#include <cstdint>

namespace CardenzaHAL::SystemMenu::launcher {

enum class ChargeState : uint8_t { Unknown, Idle, Charging, Full };
constexpr int validatedAutoSleepSeconds(int value) {
  return value >= 0 && value <= 1800 ? value : 60;
}

constexpr uint32_t kPowerHoldMs = 2000, kPowerTapMs = 350, kPowerOffFadeMs = 600;
constexpr uint32_t kPowerReturnMs = 300;
enum class PowerKeyEvent { None, Press, Tap };
// A wake press is consumed until release. Taps go Back only on release, so
// holding Fn cannot walk through menus before its sleep animation appears.
struct PowerKeyGesture {
  bool raw, stable, armed, active = false;
  uint32_t changed, since = 0;
  PowerKeyGesture(bool pressed = false, uint32_t now = 0)
      : raw(pressed), stable(pressed), armed(!pressed), changed(now) {}
  uint32_t heldMs(uint32_t now) const { return active ? uint32_t(now - since) : 0; }
  bool complete(uint32_t now) const { return active && raw && stable && heldMs(now) >= kPowerHoldMs; }
  PowerKeyEvent update(bool pressed, uint32_t now) {
    if (pressed != raw) { raw = pressed; changed = now; }
    if (stable != raw && uint32_t(now - changed) >= 35) {
      stable = raw;
      if (!stable) {
        const bool tap = active && heldMs(now) < kPowerTapMs;
        active = false; armed = true;
        return tap ? PowerKeyEvent::Tap : PowerKeyEvent::None;
      }
      if (armed) {
        active = true; since = now;
        return PowerKeyEvent::Press;
      }
    }
    return PowerKeyEvent::None;
  }
};
inline float powerHoldFill(uint32_t elapsed) {
  const float p = elapsed < kPowerHoldMs ? float(elapsed) / kPowerHoldMs : 1.f;
  // Cubic ease-in-out gives the sliding sheet a distinct acceleration and stop.
  const float remaining = 1.f - p;
  return p < .5f ? 4.f * p * p * p : 1.f - 4.f * remaining * remaining * remaining;
}
inline float powerReturnFill(float from, uint32_t elapsed) {
  const uint32_t time = elapsed < kPowerReturnMs ? elapsed : kPowerReturnMs;
  return from*(1-powerHoldFill(time*kPowerHoldMs/kPowerReturnMs));
}
inline uint8_t powerBootBrightness(int configured, float progress) {
  const int target = configured < 0 ? 0 : configured > 100 ? 100 : configured;
  const int peek = target < 35 ? target : 35;
  const float p = progress < 0 ? 0 : progress > 1 ? 1 : progress;
  return uint8_t(peek + (target - peek) * p + .5f);
}
inline uint8_t powerOffBrightness(int configured, uint32_t elapsed) {
  const int target = configured < 0 ? 0 : configured > 100 ? 100 : configured;
  return elapsed >= kPowerOffFadeMs ? 0 : uint8_t(target * (kPowerOffFadeMs - elapsed) / kPowerOffFadeMs);
}
inline bool powerPeekExpired(uint32_t elapsed, ChargeState charge, bool held) {
  return !held && elapsed >= (charge == ChargeState::Charging ? 12000u : 8000u);
}
// PCA9555 CHRG/FULL are active low. Both asserted is not a valid status.
constexpr ChargeState launcherChargeState(uint16_t inputs, bool valid) {
  if (!valid)
    return ChargeState::Unknown;
  bool charging = !(inputs & (1u << 2)), full = !(inputs & (1u << 3));
  if (charging && full)
    return ChargeState::Unknown;
  return charging ? ChargeState::Charging
         : full   ? ChargeState::Full
                  : ChargeState::Idle;
}

}  // namespace CardenzaHAL::SystemMenu::launcher
