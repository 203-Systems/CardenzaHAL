// The USB network (usb/usb.h, Config::network): the board as a CDC-NCM network adapter. A stub until the
// network side is in: no descriptor, no class.
#include "usb/internal/dev.h"

namespace CardenzaHAL::USB::detail {

std::size_t NetworkDescriptor(uint8_t*, std::size_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t) { return 0; }
const char* NetworkMacString() { return ""; }

}  // namespace CardenzaHAL::USB::detail
