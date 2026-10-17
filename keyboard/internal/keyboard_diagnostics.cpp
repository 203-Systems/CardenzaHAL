#include "keyboard_driver.h"

namespace CardenzaHAL::Keyboard {

void KeyboardDriver::setDiagDirect(bool enable, int en_level, uint8_t col, uint8_t row)
{
    _diag_en_level = en_level;
    _diag_col = col;
    _diag_row = row;
    _diag_scan_row = 0;
    _diag_direct_mode = enable;
}

}  // namespace CardenzaHAL::Keyboard
