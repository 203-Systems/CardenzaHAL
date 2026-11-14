// The web keyboard (web/hal/keyboard.cpp) on the host kernel: the page's keys go through the HAL's own
// debouncer and four-key guard, the list keeps the device's order, LEDs read back from the scan masks.
#include <cassert>
#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyboard/keyboard.h"

#include "sim_clock.h"
#include "web/hal/web_keyboard.h"

namespace K = CardenzaHAL::Keyboard;
namespace W = cardenza_web::keyboard;

int main()
{
    sim::Start();
    assert(K::init());
    sim::RunUntil(1000);
    W::SetKey(3, 2, true);
    sim::RunUntil(2000);  // not settled after 1 ms
    K::updateKeyList();
    assert(K::isPressed() == 0);
    sim::RunUntil(6000);  // settled after the 3 ms debounce
    K::updateKeyList();
    assert(K::isPressed() == 1 && K::keyList()[0].x == 3 && K::keyList()[0].y == 2);
    assert(K::isKeyPressing(2 * 14 + 3 + 1) && K::isChanged() && !K::isChanged());

    // The device's order: odd columns (rows 0..3 of the scan) before even ones.
    W::SetKey(2, 0, true);
    W::SetKey(13, 3, true);
    sim::RunUntil(12000);
    K::updateKeyList();
    assert(K::isPressed() == 3);
    assert(K::keyList()[0].x == 13 && K::keyList()[1].x == 3 && K::keyList()[2].x == 2);

    // Five keys at once: the guard holds the newcomers back until they are let go.
    W::SetKey(5, 1, true);
    W::SetKey(7, 1, true);
    sim::RunUntil(20000);
    K::updateKeyList();
    assert(K::isPressed() == 3);
    W::ReleaseAll();
    sim::RunUntil(30000);
    K::updateKeyList();
    assert(K::isPressed() == 0);

    // LEDs: levels as set while enabled, nothing while disabled; the global level as given.
    K::setLED({4, 1}, 9);
    assert(W::Leds().levels[1 * 14 + 4] == 0 && W::Leds().enabled == 0);
    K::enableLED(true);
    assert(W::Leds().levels[1 * 14 + 4] == 9);
    K::setLEDFrame({});
    assert(W::Leds().levels[1 * 14 + 4] == 0);
    K::setAllLEDs(20);
    assert(W::Leds().levels[0] == 15 && W::Leds().levels[55] == 15);
    K::setLEDGlobalBrightness(7);
    assert(W::Leds().global == 7 && K::ledGlobalBrightness() == 7);
    std::printf("web keyboard tests passed\n");
    return 0;
}
