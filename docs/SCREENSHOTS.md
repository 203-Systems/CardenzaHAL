# Screenshots

[中文](SCREENSHOTS.zh-CN.md)

Every firmware on CardenzaHAL takes screenshots the same way, and so does the 203 Launcher (the user, 2026-11-24:
"给HAL加入一个截图功能。会把内容保存到SD卡里面的固定位置/screenshot，并且可以在社区的个人主页里面访问。" and "按住Home键加右下角
空格键截屏 … 确保所有固件是统一的"). A firmware needs no code for it: `CardenzaHAL::Init` starts it
(`CONFIG_CARDENZA_HAL_SCREENSHOT`, on by default; `screenshot/screenshot.h`).

## Taking one

- **HOME + space** (the side key and the bottom-right key, printed as an open box): in either order, together, or
  one just after the other (within 200 ms, `Logic::kComboWindowMs`: a stiff key pressed a little late still
  counts). Each press of space while HOME stays down takes one more.
- **Over USB**: `203cli board screenshot` (saves it on the card and writes the PNG on the computer; `--no-save`
  only returns it), the development channel's `SCREENSHOT` (`0x12`, [usb-dev.md](usb-dev.md)) on every way the
  system channel goes (HID, SysEx, WebUSB, the serial port), and the serial port's word `screenshot` (one JSON
  line with the file's name).
- **From the firmware**: `CardenzaHAL::Screenshot::Take()` (returns at once; `SetListener` says how it went).

### HOME and the system menu

Since screenshots, the system menu opens when HOME is **let go**, after a short press (30 ms to 1 s) that took no
screenshot, 200 ms after the release (a space in that moment makes it a screenshot instead). A longer press opens
nothing. A HOME already down when the firmware starts or when the menu closes counts for nothing until it is let
go, as before.

| Keys | What happens |
| --- | --- |
| HOME pressed and let go within 1 s, nothing else | the menu, 200 ms after the release |
| HOME held, space pressed | a screenshot at once; HOME's release opens nothing |
| space held (any time), HOME pressed | a screenshot |
| both at the same moment | a screenshot |
| HOME tapped, space within 200 ms after | a screenshot, no menu |
| space tapped, HOME within 200 ms after | a screenshot |
| HOME tapped, space more than 200 ms later | the menu (space is the firmware's) |
| HOME held 1 s or longer, nothing else | nothing |

`tests/screenshot_test.cpp` runs each of these on the gesture (`Logic::HomeGesture`).

### Space and the firmware

- **Space after HOME** (HOME down, or let go less than 200 ms ago) is the system's: the firmware never sees that
  press (`updateKeyList` hides it until it is let go).
- **Space before HOME** has reached the firmware already: it gets space at once, as always, because a music
  firmware's PLAY must not wait; when HOME follows within the window, the screenshot is taken all the same, after
  the firmware acted on space. In Cardenza Synth, space is PLAY: pressing space first starts (or stops) the
  transport, then HOME takes the screenshot; HOME first takes the screenshot without touching the transport.
- **Opt-in hold-back**: a firmware where space is not time-critical calls `Screenshot::SetSpaceWait(true)`. Then
  every press of space waits 200 ms before it reaches the firmware; one that becomes a screenshot never does, a
  tap shorter than the window arrives as a 30 ms tap after it.
- **HOME**: with the system menu on, HOME is the system's and the firmware never sees it
  (`System::GetHomeButton` stays false). A firmware that turned the menu off (`SystemMenu::Enable(false)`) gets
  HOME as before, and HOME + space is still the screenshot (all firmware take screenshots the same way).

## What is taken

The screen's exact 240 x 135 pixels as the firmware shows them, never the system menu or the effect:

| The firmware draws with | What the screenshot is |
| --- | --- |
| `Display::Present`, `PresentChanged`, the frame presenter, `DoubleBuffer` | the frame it presented last (kept since HOME went down; over USB and from `Take`, the next one it presents, or the last one when none comes within 250 ms) |
| the panel's handles itself (`esp_lcd_panel_draw_bitmap`: LVGL, its own code) | what it drew, copied by the HAL as it drew (a 65 KB copy in PSRAM, made at its first draw; without PSRAM it cannot be captured) |
| raw SPI transfers of its own (`esp_lcd_panel_io_tx_color`) | cannot be captured: the card says so ("Can't capture the screen"); USB answers `no framebuffer` |

The panel has no read line (its MISO is not wired), so the HAL cannot read the screen back: it keeps what was sent.

## The file

- `/sdcard/screenshot/2026-11-24_153012_CardenzaSynth.png` when the board's clock is set (UTC; a second shot in the
  same second `…153012-2_…`), else `shot-0001_CardenzaSynth.png` (one more than the highest number in the folder).
  The clock is set by the presence service from the community's answers (`"now"`) once the board has been online
  in this run, or by the firmware itself. The firmware's part is its project name (`[A-Za-z0-9-]`, at most 32
  characters); the Launcher's is `Launcher`.
- A PNG of the exact pixels (`screenshot/png.h`): RGB565 widened to 8 bits a channel (r8 = r5 << 3 | r5 >> 2, so
  the 565 value can be read back), an indexed PNG when the screen has 256 colours or fewer (most UI art: 1–10 KB),
  8-bit RGB otherwise (110 KB at worst).
- Written as `.<name>.tmp`, synced, then renamed: a power cut leaves no half file (a leftover `.tmp` is removed at
  the next screenshot). No card: nothing is written and the card says so ("No SD card").
- `/sdcard/screenshot/` is the third folder at the card's root beside `/Launcher/` and `/Apps/`
  ([SD-CARD.md](SD-CARD.md)): the user's, shared by every firmware and the Launcher.

## The effect

The same in every firmware and in the Launcher (`screenshot/card.h`, drawn with the Launcher's own drawing code as
the system menu is): a white flash that fades out through a 4 x 4 ordered dither (180 ms), then a white card at the
bottom with the shot at a quarter of its size and what happened ("Screenshot saved" and the file's name, or "No SD
card" / "Card write failed" / "Not enough memory" with "Not saved"), in the board's language, coming and going
through the same dither; 1.6 s in all. Fades only; every pixel is the firmware's, the flash's white or the card's,
never a mix.

The firmware keeps running meanwhile: its sound, its keys and its frames (the card is drawn over its latest ones,
only what changed is sent). When the card has gone, the firmware's picture comes back as it is and its next frame
is sent whole. A firmware that draws through the panel's handles itself should hold its drawing while
`Screenshot::IsShowing()`, as it does while the system menu is open.

## The upload

- **On by default**, the board setting `screenshot_upload` (HOME > Settings > Upload screenshots, whose detail says
  "HOME + space takes one"; `203cli board settings set screenshot_upload off`; [SETTINGS.md](SETTINGS.md)). Shots
  taken while it is off stay on the card only.
- Each saved shot is queued in `/sdcard/screenshot/.upload-queue`, a journal (`Q <name>` queued, `U <name>`
  uploaded, `S <name>` skipped for good; the oldest pending first; removed when nothing is pending). The
  Launcher reads and writes the same file: a shot taken in a firmware may be uploaded by the Launcher later.
- In a firmware, the presence service ([net/presence.h](../net/presence.h)) uploads them: only when the board is
  bound to an account and online through it (bound by the Launcher, a saved network, Receive firmware on), after
  each of its inbox requests, up to 4 at a time, on its task of priority 1, never while the firmware's activity
  (`Presence::SetActivity`) or late audio holds it back. `POST <api>/device/screenshots` with the device token,
  the PNG as the body (at most 256 KiB), `X-Screenshot-Name`, `X-Taken-At` (when the clock was set), the firmware's
  name, version and image digest.
- What the community answers: stored or already there → `U`; refused as not a PNG or too large → `S`; the account
  is full (`quota_full`) or the community has no such route → nothing more until the next start; anything else →
  the next time. The device token never goes elsewhere and is never logged.
- **Private by default**: the person sees them on their profile page in the community (截图 / Screenshots) and
  chooses which to make public.
- **Without Wi-Fi**: the community's website reads the card over USB and uploads what the account lacks.

## Costs

Nothing at rest but a look at space in each `updateKeyList` (a few instructions) and HOME's task, shared with the
system menu. A shot: a 65 KB copy of the frame and the encoder's 32 KB (PSRAM when there is some; without PSRAM
the encoder works in internal RAM only while 48 KB stay free beside it, and reads the firmware's framebuffer
where it is), on a task of priority 1 with a 6 KB stack, gone 10 s after the last shot; encoding takes some 20–80
ms there. The effect: a 4 KB thumbnail and an 11 KB card (PSRAM when there is some), for 1.6 s. Flash: about 18 KB. The upload: one more request of the presence service per shot.

## For a firmware

```cpp
#include "screenshot/screenshot.h"

CardenzaHAL::Screenshot::SetListener([](const CardenzaHAL::Screenshot::Shot& shot, void*) {
    // shot.status: Saved, NoCard, WriteFailed, NoFrame, NoMemory; shot.name in /sdcard/screenshot/
}, nullptr);
CardenzaHAL::Screenshot::Take();                    // as HOME + space
CardenzaHAL::Screenshot::Take({.save = true, .effect = false});
CardenzaHAL::Screenshot::SetSpaceWait(true);        // only where space is not time-critical
```

For AI agents developing a firmware: `203cli board screenshot --no-save --out /tmp/screen.png --json` grabs the
screen in one command (`board screen` gives the raw frame with no effect and nothing saved).
