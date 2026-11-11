#include "keyboard/internal/key_debounce.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

namespace {
using CardenzaHAL::Keyboard::detail::KeyDebouncer;
using Rows = std::array<uint8_t, 8>;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
unsigned Count(const Rows& rows)
{
  unsigned count = 0;
  for (unsigned bit = 0; bit < 56; ++bit) count += (rows[bit / 7] >> (bit % 7)) & 1;
  return count;
}
struct Keyboard {
  KeyDebouncer keys;
  uint8_t raw[8]{};
  uint32_t now = 1000;
  bool Poll(uint32_t elapsed = 0) { now += elapsed; return keys.Update(raw, now); }
  void Settle() { Poll(); Poll(KeyDebouncer::kDebounceUs); Poll(KeyDebouncer::kDebounceUs); }
  void Physical(int x, int y, bool down)
  {
    const unsigned row = 3 - y + (x % 2 == 0 ? 4 : 0);
    const uint8_t bit = 1u << (x / 2);
    if (down) raw[row] |= bit;
    else raw[row] &= ~bit;
  }
};

void LegitimateChordsAndLimit()
{
  Keyboard keyboard;
  for (int x = 1; x <= 3; ++x) keyboard.Physical(x, 3, true);
  keyboard.Physical(0, 2, true); // FN plus three notes.
  keyboard.Settle();
  Require(Count(keyboard.keys.Rows()) == 4 && !keyboard.keys.InterferenceActive(),
          "normal chord plus modifier was restricted");
  const auto held = keyboard.keys.Rows();
  keyboard.Physical(0, 1, true); // TAB would be key five.
  keyboard.Settle();
  Require(keyboard.keys.Rows() == held && keyboard.keys.InterferenceActive() && !keyboard.keys.Pending(),
          "excess keys leaked through or caused perpetual task work");
  keyboard.Physical(0, 2, false);
  keyboard.Settle();
  Require(Count(keyboard.keys.Rows()) == 3 && !(keyboard.keys.Rows()[6] & 1),
          "release was lost or rejected TAB appeared after the count fell");
  Keyboard rectangle;
  rectangle.raw[0] = rectangle.raw[4] = 3;
  rectangle.Settle();
  Require(Count(rectangle.keys.Rows()) == 4 && !rectangle.keys.InterferenceActive(),
          "a legitimate four-key chord was treated as a ghost rectangle");
}

void NoiseBurstPreservesReleases()
{
  Keyboard keyboard;
  keyboard.raw[0] = 3;
  keyboard.Settle();
  std::fill(std::begin(keyboard.raw), std::end(keyboard.raw), 0x7f);
  keyboard.Settle();
  Require(Count(keyboard.keys.Rows()) == 2 && keyboard.keys.Rows()[0] == 3, "noise created extra notes");
  keyboard.raw[0] &= ~1u;
  keyboard.Settle();
  Require(Count(keyboard.keys.Rows()) == 1 && keyboard.keys.Rows()[0] == 2,
          "confirmed note-off was blocked during interference");
  std::fill(std::begin(keyboard.raw), std::end(keyboard.raw), 0);
  keyboard.raw[0] = 2;
  keyboard.raw[3] = 0x07; // Three previously blocked keys stay held.
  keyboard.Settle();
  Require(Count(keyboard.keys.Rows()) == 1 && !keyboard.keys.Pending(),
          "noise clearing injected delayed notes or kept waking the task");
  keyboard.raw[3] = 0;
  keyboard.Settle();
  keyboard.raw[3] = 0x07;
  keyboard.Settle();
  Require(Count(keyboard.keys.Rows()) == 4 && !keyboard.keys.InterferenceActive(),
          "fresh presses did not recover after a stable release");
}

void ReleaseBounceAndClockWrap()
{
  Keyboard keyboard;
  keyboard.now = (std::numeric_limits<uint32_t>::max)() - 1000;
  std::fill(std::begin(keyboard.raw), std::end(keyboard.raw), 0x7f);
  keyboard.Settle();
  Require(Count(keyboard.keys.Rows()) == 0 && !keyboard.keys.Pending(), "boot-time noise was accepted");
  std::fill(std::begin(keyboard.raw), std::end(keyboard.raw), 0);
  keyboard.raw[2] = 1;
  keyboard.Settle();
  Require(keyboard.keys.InterferenceActive() && !keyboard.keys.Pending(), "held key lost quarantine");
  keyboard.raw[2] = 0;
  keyboard.Poll();
  keyboard.Poll(2999);
  keyboard.raw[2] = 1;
  keyboard.Poll(1);
  keyboard.Settle();
  Require(keyboard.keys.InterferenceActive() && Count(keyboard.keys.Rows()) == 0,
          "release bounce unlocked a rejected key");
  keyboard.raw[2] = 0;
  keyboard.now = (std::numeric_limits<uint32_t>::max)() - 999;
  keyboard.Poll();
  Require(keyboard.keys.Pending(), "quarantine release did not request processing");
  keyboard.Poll(3000);
  Require(!keyboard.keys.InterferenceActive() && !keyboard.keys.Pending(), "clock wrap broke release");
  keyboard.raw[2] = 1;
  keyboard.Settle();
  Require(Count(keyboard.keys.Rows()) == 1, "re-press after quarantine failed");
}

void IndependentRowPublication()
{
  CardenzaHAL::Keyboard::detail::KeyCountGuard guard;
  Rows debounced{};
  debounced[0] = 3; debounced[1] = 3;
  Require(guard.Publish(debounced), "initial confirmed keys were lost");
  debounced[2] = 1;
  Require(!guard.Publish(debounced) && Count(guard.Accepted()) == 4,
          "independent row timing published more than four keys");
  debounced[0] = 1;
  Require(guard.Publish(debounced) && Count(guard.Accepted()) == 3,
          "publication lost old releases or admitted a quarantined key");
}

void RandomScans()
{
  Keyboard keyboard;
  std::mt19937 random(203);
  for (unsigned frame = 0; frame < 20000; ++frame)
  {
    const auto previous = keyboard.keys.Rows();
    for (unsigned i = 0, flips = random() % 12; i < flips; ++i)
    {
      const unsigned key = random() % 56;
      keyboard.raw[key / 7] ^= 1u << (key % 7);
    }
    unsigned rawCount = 0;
    for (const auto row : keyboard.raw)
      for (unsigned column = 0; column < 7; ++column) rawCount += (row >> column) & 1;
    keyboard.Poll(random() % 5000);
    const auto& reported = keyboard.keys.Rows();
    Require(Count(reported) <= 4, "random scan exceeded the published limit");
    if (rawCount > 4)
      for (unsigned row = 0; row < 8; ++row)
        Require(!(reported[row] & ~previous[row]), "excess count injected a new press");
  }
  std::fill(std::begin(keyboard.raw), std::end(keyboard.raw), 0);
  keyboard.Settle();
  Require(Count(keyboard.keys.Rows()) == 0 && !keyboard.keys.Pending() && !keyboard.keys.InterferenceActive(),
          "noise left a stuck key or pending task work after release");
  std::fill(std::begin(keyboard.raw), std::end(keyboard.raw), 0x80);
  keyboard.Settle();
  Require(Count(keyboard.keys.Rows()) == 0 && !keyboard.keys.InterferenceActive(), "unused bits triggered keys");
}
}

int main()
{
  try
  {
    LegitimateChordsAndLimit();
    NoiseBurstPreservesReleases();
    ReleaseBounceAndClockWrap();
    IndependentRowPublication();
    RandomScans();
    std::cout << "key count interference guard passed\n";
  }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
