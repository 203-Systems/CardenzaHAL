// Copied from the 203 Launcher (https://github.com/203-Systems/203-launcher, MIT; LICENSE and
// NOTICE.md there) by tools/launcher_ui.py: src/platform/ui_motion.h at 4409843d25be.
// Do not edit: change it in the Launcher and copy it again.
#pragma once

#include <algorithm>
#include <cstdint>

namespace CardenzaHAL::SystemMenu::launcher {

constexpr uint8_t uiBackIconGrey(bool held) { return held ? 128 : 255; }
// Crossfade the highlight along the four bars; disconnected bars stay gray.
inline uint8_t uiWifiBarGrey(unsigned bar, unsigned level, bool connecting, uint32_t now) {
  if (!connecting) return level ? (bar < level ? 235 : 68) : 125;
  const unsigned phase = (now % 800 + 800 - bar * 200) % 800;
  const unsigned pulse = phase < 200 ? phase : phase < 400 ? 400 - phase : 0;
  return uint8_t(80 + pulse * 155 / 200);
}
inline bool uiSplashExpired(uint32_t shown, uint32_t now, uint32_t timeout) {
  return timeout && uint32_t(now - shown) >= timeout;
}
// A held wake key must be released before a fresh press can enter Launcher.
struct UiPressEdge {
  bool raw, stable, armed;
  uint32_t changed;
  UiPressEdge(bool initiallyPressed, uint32_t now)
      : raw(initiallyPressed), stable(initiallyPressed),
        armed(!initiallyPressed), changed(now) {}
  bool update(bool pressed, uint32_t now) {
    if (pressed != raw) {
      raw = pressed;
      changed = now;
    }
    if (pressed == stable || uint32_t(now - changed) < 35)
      return false;
    stable = pressed;
    if (!pressed) {
      armed = true;
      return false;
    }
    if (!armed)
      return false;
    armed = false;
    return true;
  }
};
inline int uiSelectionDelta(bool left, bool up, bool right, bool down) {
  return int(right || down) - int(left || up);
}

// Held direction keys repeat after a pause, then speed up for long lists.
// Select and Back never repeat: one press, one action.
constexpr uint32_t kKeyRepeatDelayMs = 400, kKeyRepeatMs = 110,
                   kKeyRepeatFastAfterMs = 1500, kKeyRepeatFastMs = 50;
inline bool uiKeyRepeatDue(uint32_t heldMs, uint32_t sinceLastMs) {
  return heldMs >= kKeyRepeatDelayMs &&
         sinceLastMs >= (heldMs >= kKeyRepeatFastAfterMs ? kKeyRepeatFastMs : kKeyRepeatMs);
}

// Smoothstep in both directions: velocity reaches zero at each reversal.
inline float uiIndeterminateTravel(uint32_t now) {
  const float phase = float(now % 1600) / 800.f;
  const float p = phase < 1.f ? phase : 2.f - phase;
  return p * p * (3.f - 2.f * p);
}

// Fade through the background: never draw two titles over the same pixels.
inline float homeTitleOpacity(float progress, bool incoming) {
  return std::clamp(incoming ? (progress - .10f) / .23f
                             : 1.f - progress / .10f, 0.f, 1.f);
}

}  // namespace CardenzaHAL::SystemMenu::launcher
