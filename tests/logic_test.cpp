#include "display/backlight_brightness.h"
#include "keyboard/internal/key_debounce.h"
#include "keyboard/internal/led_frame.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void Require(bool passed, const char* message)
{
  if (!passed) throw std::runtime_error(message);
}

void TestDebounceEdgesAndIdle()
{
  using CardenzaHAL::Keyboard::detail::KeyDebouncer;
  KeyDebouncer keys;
  uint8_t raw[8]{};
  Require(!keys.Update(raw, 100) && !keys.Pending(), "idle keyboard requested more processing");

  raw[2] = 0x05;
  Require(!keys.Update(raw, 1000) && keys.Pending(), "chord bypassed initial debounce");
  Require(!keys.Update(raw, 3999) && keys.Pending(), "chord settled before 3 ms");
  Require(keys.Update(raw, 4000) && !keys.Pending() && keys.Rows()[2] == 0x05,
          "chord did not settle at 3 ms");
  for (uint32_t now = 4001; now < 24000; now += 137)
    Require(!keys.Update(raw, now) && !keys.Pending(), "held chord needed a rebuild or debounce wakeup");

  raw[2] = 0;
  Require(!keys.Update(raw, 25000) && keys.Pending(), "release bypassed debounce");
  raw[2] = 0x05;  // Contact bounced back before the release became stable.
  Require(!keys.Update(raw, 26000) && !keys.Pending(), "cancelled release stayed pending");
  raw[2] = 0;
  Require(!keys.Update(raw, 27000) && keys.Pending(), "new release was lost after bounce");
  Require(!keys.Update(raw, 29999) && keys.Pending(), "release ignored the restarted interval");
  Require(keys.Update(raw, 30000) && !keys.Pending() && keys.Rows()[2] == 0,
          "debounced release was lost");
}

void TestDebounceIndependentRowsAndClockWrap()
{
  using CardenzaHAL::Keyboard::detail::KeyDebouncer;
  KeyDebouncer keys;
  uint8_t raw[8]{};
  raw[0] = 1;
  Require(!keys.Update(raw, 1000), "first row bypassed debounce");
  raw[7] = 0x40;
  Require(!keys.Update(raw, 2000), "second row bypassed debounce");
  Require(keys.Update(raw, 4000) && keys.Rows()[0] == 1 && keys.Rows()[7] == 0 && keys.Pending(),
          "pending row delayed or overwrote another row");
  Require(keys.Update(raw, 5000) && keys.Rows()[7] == 0x40 && !keys.Pending(),
          "independent row did not settle");
  raw[0] = 2;  // Same key count, different key.
  Require(!keys.Update(raw, 6000) && keys.Pending(), "equal-count key replacement was not observed");
  Require(keys.Update(raw, 9000) && keys.Rows()[0] == 2, "equal-count replacement was lost");

  KeyDebouncer wrap;
  uint8_t nearWrap[8]{};
  nearWrap[3] = 0x0f;
  const uint32_t start = (std::numeric_limits<uint32_t>::max)() - 999;
  Require(!wrap.Update(nearWrap, start) && wrap.Pending(), "wrap fixture bypassed debounce");
  Require(!wrap.Update(nearWrap, start + uint32_t{2999}), "clock wrap shortened debounce");
  Require(wrap.Update(nearWrap, start + uint32_t{3000}) && wrap.Rows()[3] == 0x0f && !wrap.Pending(),
          "clock wrap lost held keys");
}

void TestEveryLedCoordinateAndDuty()
{
  using namespace CardenzaHAL::Keyboard;
  for (int y = 0; y < 4; ++y)
  {
    for (int x = 0; x < 14; ++x)
    {
      for (uint8_t level = 0; level < 16; ++level)
      {
        detail::LedFrameState lights;
        lights.SetKey({x, y}, level);
        const auto masks = lights.BuildScanMasks();
        const int physicalRow = 3 - y + (x % 2 == 0 ? 4 : 0);
        const uint8_t physicalBit = static_cast<uint8_t>(1u << (1 + x / 2));
        unsigned duty = 0;
        for (int phase = 0; phase < 4; ++phase)
        {
          for (int row = 0; row < 8; ++row)
          {
            const uint8_t expected = row == physicalRow && (level & (1u << phase)) ? physicalBit : 0;
            Require(masks[phase][row] == expected, "LED wiring or four-plane mapping changed");
          }
          if (masks[phase][physicalRow]) duty += 1u << phase;
        }
        Require(duty == level, "LED no longer has exactly sixteen electrical duties");
      }
    }
  }
}

void TestBatchMatchesLegacyAndSkipsUnchangedFrames()
{
  using namespace CardenzaHAL::Keyboard;
  detail::LedFrameState batch;
  detail::LedFrameState singles;
  LedFrame levels{};
  Require(!batch.Assign(levels) && !batch.Fill(0), "blank frame was not idempotent");
  for (std::size_t index = 0; index < levels.size(); ++index)
  {
    levels[index] = static_cast<uint8_t>((index * 7) % 19);
    singles.SetKey({static_cast<int>(index % 14), static_cast<int>(index / 14)}, levels[index]);
  }
  Require(batch.Assign(levels), "changed batch was ignored");
  Require(batch.BuildScanMasks() == singles.BuildScanMasks(), "batch and legacy LED output differ");
  Require(!batch.Assign(levels), "identical batch requested another scan-table publication");
  Require(!batch.SetKey({-1, 0}, 15) && !batch.SetKey({14, 0}, 15) && !batch.SetKey({0, 4}, 15),
          "invalid key changed LED state");
  Require(batch.Fill(255) && !batch.Fill(15), "full-frame brightness did not clamp or skip repeats");
  for (const auto& plane : batch.BuildScanMasks())
    for (const auto row : plane) Require(row == 0xfe, "maximum brightness enabled a non-key bit");
  Require(!batch.SetKey({0, 0}, 255), "single-key brightness did not clamp before comparison");
  Require(batch.Fill(0), "clear did not change an illuminated frame");
  for (const auto& plane : batch.BuildScanMasks())
    for (const auto row : plane) Require(row == 0, "clear left an LED enabled");
}

void TestBacklightGamma()
{
  using CardenzaHAL::Display::detail::BacklightDuty;
  unsigned previous = 0;
  for (unsigned level = 0; level <= 255; ++level)
  {
    const unsigned actual = BacklightDuty(static_cast<uint8_t>(level));
    const unsigned rounded = static_cast<unsigned>(std::lround(std::pow(level / 255.0, 2.2) * 4095));
    const unsigned expected = level == 0 ? 0 : (rounded == 0 ? 1 : rounded);
    Require(actual == expected && actual >= previous && actual <= 4095,
            "backlight curve is not monotonic, bounded gamma 2.2");
    Require(level == 0 || actual != 0, "nonzero backlight unexpectedly turns off");
    previous = actual;
  }
  Require(BacklightDuty(0) == 0 && BacklightDuty(255) == 4095 && BacklightDuty(128) == 899,
          "backlight endpoints or mid-brightness changed");
  for (unsigned level = 10; level < 255; ++level)
    Require(BacklightDuty(static_cast<uint8_t>(level + 1)) > BacklightDuty(static_cast<uint8_t>(level)),
            "two selectable screen settings collapsed to the same duty");
}

}  // namespace

int main()
{
  try
  {
    TestDebounceEdgesAndIdle();
    TestDebounceIndependentRowsAndClockWrap();
    TestEveryLedCoordinateAndDuty();
    TestBatchMatchesLegacyAndSkipsUnchangedFrames();
    TestBacklightGamma();
    std::cout << "HAL debounce, LED frame compatibility and backlight gamma passed\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
