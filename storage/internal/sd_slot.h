#pragma once

#include <driver/sdmmc_host.h>

#include "board/pins.h"

// The SD card's slot: 4-bit, the board's pins. One place for the file system's mount (storage/storage.cpp) and the
// USB drive's raw card (usb/drive.cpp). Internal to the HAL.
namespace CardenzaHAL::Storage::detail {

    inline sdmmc_slot_config_t SlotConfig()
    {
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.clk = static_cast<gpio_num_t>(Pins::kSdClk);
        slot.cmd = static_cast<gpio_num_t>(Pins::kSdCmd);
        slot.d0 = static_cast<gpio_num_t>(Pins::kSdDat0);
        slot.d1 = static_cast<gpio_num_t>(Pins::kSdDat1);
        slot.d2 = static_cast<gpio_num_t>(Pins::kSdDat2);
        slot.d3 = static_cast<gpio_num_t>(Pins::kSdDat3);
        slot.width = 4;
        return slot;
    }

}  // namespace CardenzaHAL::Storage::detail
