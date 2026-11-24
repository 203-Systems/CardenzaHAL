#pragma once

#include <cstddef>
#include <cstdint>

#include "screenshot/card.h"
#include "screenshot/logic.h"

// What the rest of the HAL calls of the screenshots (screenshot/screenshot.h). Internal to the HAL.
namespace CardenzaHAL::Screenshot::detail {

    // ---- The side key's task (system/menu.cpp) ------------------------------------------------------------
    // The gesture's state for the keyboard's look at space (published after each of its looks).
    void PublishWindow(const Logic::HomeGesture::Window& window);
    // A press of space the keyboard took for the system since the last call (once).
    bool TakeClaimedSpace();
    // HOME + space happened: take a screenshot of `frame` (the firmware's frame the menu's capture kept since HOME
    // went down; nullptr: its last framebuffer, or its panel draws). Copies the frame and returns (a few ms); the
    // file is written on the screenshots' own task.
    void FromKeys(const uint16_t* frame);

    // The effect to show (posted by whoever took the screenshot), taken by the side key's task, which draws it.
    struct EffectPost {
        uint16_t* thumb = nullptr;  // 60 x 34 (Logic::Effect::kThumb), the task's to free (heap_caps_free); or null
        Card::Words words = Card::Words::Taken;
        char name[84] = "";
    };
    bool TakeEffect(EffectPost& post);
    // What the card says now (it changes when the file is written): bumped each time; `post` gets the words.
    uint32_t EffectRevision(EffectPost& post);
    // The effect is on the screen (IsShowing): set by the side key's task.
    void SetShowing(bool showing);

    // ---- USB (usb/dev.cpp, SCREENSHOT 0x12) ---------------------------------------------------------------
    struct UsbOut {
        bool (*write)(void* context, const void* data, std::size_t length);
        void* context;
    };
    // Takes one (Logic::Usb flags) and writes the answer (Logic::Usb::Header, then the data) through `out`. On the
    // development channel's worker (priority 1). nullptr: answered; otherwise the error, nothing written yet.
    const char* TakeForUsb(uint8_t flags, const UsbOut& out);

}  // namespace CardenzaHAL::Screenshot::detail
