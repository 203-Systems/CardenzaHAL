// The USB drive (usb/usb.h, Config::drive): the SD card lent to the computer as mass storage.
#include "usb/internal/dev.h"
#include <cstring>
#include "tusb.h"

namespace CardenzaHAL::USB::detail {

#if CFG_TUD_MSC
std::size_t DriveDescriptor(uint8_t* out, std::size_t capacity, uint8_t itf, uint8_t stringIndex, uint8_t epOut, uint8_t epIn)
{
    const uint8_t block[] = {TUD_MSC_DESCRIPTOR(itf, stringIndex, epOut, epIn, 64)};
    if (sizeof(block) > capacity) return 0;
    std::memcpy(out, block, sizeof(block));
    return sizeof(block);
}
#else
std::size_t DriveDescriptor(uint8_t*, std::size_t, uint8_t, uint8_t, uint8_t, uint8_t) { return 0; }
#endif

void DriveHostGone() {}

}  // namespace CardenzaHAL::USB::detail

#if CFG_TUD_MSC
// No medium yet: the computer sees an empty drive.
extern "C" {
void tud_msc_inquiry_cb(uint8_t, uint8_t vendor[8], uint8_t product[16], uint8_t revision[4])
{
    std::memcpy(vendor, "203     ", 8);
    std::memcpy(product, "Cardenza SD     ", 16);
    std::memcpy(revision, "1.0 ", 4);
}
bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);  // medium not present
    return false;
}
void tud_msc_capacity_cb(uint8_t, uint32_t* blockCount, uint16_t* blockSize)
{
    *blockCount = 0;
    *blockSize = 512;
}
int32_t tud_msc_read10_cb(uint8_t, uint32_t, uint32_t, void*, uint32_t) { return -1; }
int32_t tud_msc_write10_cb(uint8_t, uint32_t, uint32_t, uint8_t*, uint32_t) { return -1; }
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const[16], void*, uint16_t)
{
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
}
}
#endif
