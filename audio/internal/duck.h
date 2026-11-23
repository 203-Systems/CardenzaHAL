#pragma once

// The speaker faded out around a flash write (system/settings.cpp). Internal to the HAL.
namespace CardenzaHAL::Audio::detail {

    // true: the codec's volume steps down to silence (about 25 ms); false: back up to the volume set
    // (Audio::SetVolume's). Writing the flash stalls the caches of both cores, and with them whatever renders
    // the firmware's sound: the DMA then sends silence, a click that is not heard while the output is down.
    // Nothing happens while the codec is not running.
    void Duck(bool down);

}  // namespace CardenzaHAL::Audio::detail
