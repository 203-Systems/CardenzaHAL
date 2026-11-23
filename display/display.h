#pragma once

#include <cstddef>
#include <cstdint>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include "region.h"  // WIDTH, HEIGHT, Region

namespace CardenzaHAL::Display {

    // Idempotent. Software restarts keep the panel's pixels; cold boots reset
    // it. Brightness is staged until the first successful Present().
    bool Init();

    // Synchronous RGB565 submission from a WIDTH*HEIGHT framebuffer, including PSRAM. The HAL copies what goes
    // out into its own internal DMA blocks (display/scan_order.h: in the panel's scan order, left to right) and
    // returns when it is on the panel. On false, the caller may still reuse its framebuffer; the HAL keeps any
    // in-flight block.
    bool Present(const uint16_t* framebuffer);
    // Dirty regions refer to the full framebuffer (stride WIDTH). The first frame and recovery after a failed
    // submission (or the system menu) always send the full screen. A zero region count means unchanged pixels
    // after that first full frame.
    bool Present(const uint16_t* framebuffer, const Region* regions, std::size_t count);
    // Sends only what changed since the last frame (display/changed_regions.h), and one band of eight rows whole
    // every frame so the screen is refreshed completely about every 0.34 s at 50 fps. The framebuffer must be
    // 4-byte aligned. Its table of row hashes (4.3 KB, PSRAM when there is some) comes on first use; without it
    // every frame is sent whole. display/presenter.h tunes it, reports what each frame cost and paces a loop.
    // Do not mix with the other framebuffer Present calls in one firmware: they do not update its table.
    bool PresentChanged(const uint16_t* framebuffer);

    // Native rendering: drain the caller's queued draws and reveal the frame.
    // Do not mix a custom panel-IO callback with framebuffer Present() calls.
    bool Present();
    esp_lcd_panel_handle_t GetPanel();
    esp_lcd_panel_io_handle_t GetPanelIO();

    // Perceived brightness 0..255 (0 off). Gamma 2.2 is applied by the HAL to
    // 12-bit hardware PWM; callers must not apply another gamma correction.
    // Before the first Present(), this only stages the requested level.
    // Every display call takes the display's lock, so any task may make it; keep the frames coming from one
    // task (a DoubleBuffer's when you use one).
    bool SetBrightness(uint8_t brightness);
    // Returns the requested logical level, not the corrected PWM duty.
    uint8_t GetBrightness();
}

#include "presenter.h"  // PresentOptions, FrameStats, FramePacer, AllocateFramebuffer, DoubleBuffer
