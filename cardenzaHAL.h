#pragma once

#include "audio/audio.h"
#include "display/display.h"
#include "ir/ir.h"
#include "keyboard/keyboard.h"
#include "mic/mic.h"
#include "storage/storage.h"
#include "system/system.h"


namespace CardenzaHAL {

struct InitReport {
	bool system = false;
	bool keyboard = false;
	bool audio = false; // I2S configured and codec initialized successfully.
	bool display = false;
	bool storage = false;
	bool ir = false;
	bool overall = false;
};

bool Init();
const InitReport& LastInitReport();
}  // namespace CardenzaHAL
