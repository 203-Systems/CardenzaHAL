# The board's settings

Since 2026-11-23 (the user: "像是亮度、输出音量等等，这些应该作为全局的系统设置 … 所有使用HAL的固件都通用，能够读取。放置在Flash里面"),
the output volume, its limit, the screen's and the key lights' brightness and the language are the board's, not a firmware's: the same in every firmware on CardenzaHAL, kept in the board's flash by the HAL,
changed in the system menu (HOME > Settings) or over USB (`203cli board settings`), read and set by a firmware
through [`system/settings.h`](../system/settings.h).

| Setting (USB key) | Range | Default | What the HAL does with it |
| --- | --- | --- | --- |
| Volume (`volume`) | 0–100, by ear | 70 | the codec's volume: -60 (1 - v/100)^1.5 dB (1 is about -59 dB, 50 -21 dB, 70 -10 dB, 100 0 dB); 0 is silent |
| Volume limit (`volume_limit`) | 10–100 | 100 | the most the volume may be: every way of setting it (menu, firmware, USB) stays under it, and lowering it lowers the volume with it |
| Screen brightness (`screen_brightness`) | 10–100 % | 100 | the backlight on the 203 Launcher's duty curve and at its 256 Hz PWM: the same light in the Launcher, the menu and every firmware |
| Key lights (`key_brightness`) | 0–15 | 15 | the key lights' master level (`Keyboard::setLEDGlobalBrightness`) |
| Language (`language`) | the Launcher's codes: 0 English, 1 简体中文, 3 日本語, 4 Bahasa Indonesia, 5 Tiếng Việt | 0 | the system menu's words, and the Launcher's own language; a firmware may follow it |
| WebUSB page (`webusb_landing`) | 0 the first time only, 1 always, 2 never | 0 | whether the USB device names the 203 community's page to web browsers (the WebUSB landing page: every Chromium browser running shows a notice for it at each plug-in); from the next time a computer finds the device. In the system menu: Settings > Browser notice (网页提示) |
| Seen (`webusb_seen`) | 0, 1 | 0 | 1 once a 203 tool or the community's website has seen the board (system command 0x46, [usb-dev.md](usb-dev.md)): with `webusb_landing` 0 the page is named no more; not in the menu |

Reset (in the menu, `Settings::Reset`, `203cli board settings reset`) puts every one back to its default but the
language, as the Launcher's own reset keeps it.

## For a firmware

- `CardenzaHAL::Init` reads them and applies them before your first frame: the codec starts at the volume, the
  screen lights at the brightness, the key lights at their level. You need do nothing; do not set these yourself
  at start (`Audio::SetVolume`, `Display::SetBrightness`, `Keyboard::setLEDGlobalBrightness`): a firmware that does
  takes that part over from the setting until the user changes the setting again, so firmware made before the
  settings keep working as they did, but their users' choice is lost at every start.
- `Settings::Get(id)`, `Settings::Set(id, value)` (clamped; returns what is set), `Settings::Describe(id)` (range,
  step, default, unit), `Settings::Reset()`, `Settings::SetListener` (told after every change, on the task that made
  it). A firmware may offer its own shortcut (a volume key) with `Set`: it is applied at once and written to the
  flash a second after the last change.
- A volume of its own (a mixer's master, a project's level) is the firmware's business and stays in the firmware:
  the board's volume is the output after it.
- The microphone's gain is not a board setting (the user, 2026-11-23: a firmware's, kept and given by the
  firmware; for a few hours that day it was one, `hal_system/mic_gain`, now left unread on a board that stored
  it). A firmware that records keeps its own gain with its own settings and passes it with
  the 16-bit samples read from `Mic::InputChannel()`: `Mic::ApplyGain(samples, count, gainDb)` (`mic/gain.h`:
  -12 to +36 dB, pure, also for host tests and simulators).
- Before restarting the board yourself right after a `Set`, call `Settings::Flush()` (the system menu's restarts
  and `esp_restart`'s shutdown do it already).

## In the flash

The NVS partition the 203 Launcher shares with every firmware (label `nvs`):

| Setting | Namespace | Key | Type |
| --- | --- | --- | --- |
| Screen brightness | `launcher` | `bright` | i32, 10–100 (the Launcher's own key: 0 or missing reads as 100) |
| Language | `launcher` | `lang` | u8 (the Launcher's own key: 2 reads as 1, anything unknown as 0) |
| Volume | `hal_system` | `volume` | u8, 0–100 |
| Volume limit | `hal_system` | `volume_max` | u8, 10–100 |
| Key lights | `hal_system` | `key_light` | u8, 0–15 |
| WebUSB page | `hal_system` | `usb_landing` | u8, 0–2 |
| Seen | `hal_system` | `usb_seen` | u8, 0–1 |

A missing key is its default; a value out of its range is the default too. Key names keep their meaning: a new
meaning takes a new key.

The screen's brightness and the language are the Launcher's own values, so the Launcher and every firmware show the
same, and a Launcher that never heard of these settings keeps working unchanged: it reads `launcher/lang` from NVS
at every start (a language chosen in the menu is the Launcher's at once), and writes `launcher/bright` with its own
settings. One difference until a Launcher reads its brightness from NVS first: a Launcher with an SD card takes its
brightness from its `config.conf` there, so a brightness chosen in a firmware shows in the Launcher only after the
Launcher's own Backlight page set it again, and the Launcher writes its card's value back to NVS when it saves its
settings (203-launcher's branch `feat/system-settings` reads NVS first and resets `hal_system` with its own reset).

## When the flash is written

Writing the flash stops the caches of both cores for as long as the write lasts: a few entries take a fraction of
a millisecond, a page the NVS library erases tens of milliseconds. Code in flash stops meanwhile: the firmware's
sound (its render task: the I2S DMA then sends silence, a click), and interrupts not in IRAM (the key lights' scan:
the row it had lit stays lit at full duty, a flash of light; CardenzaSynth saw it on 2026-11-05). So the HAL:

- never writes per change: a change is applied at once and written when it has rested a second (`Set`, USB), when
  the system menu closes (what changed while it was open), at `Flush`, or at `esp_restart` (its shutdown handler);
  only what differs from what the flash holds, nothing when nothing changed;
- writes on a task of its own (priority 2, a 4 KB stack in internal RAM while it exists, gone 3 s after its last
  write), never on the firmware's;
- first fades the speaker out (the codec's volume in 12 steps, about 25 ms: `Audio::detail::Duck`), and holds the
  key lights dark from the scan's next step (`Keyboard::detail::HoldLightsDark`: the keys are still read), then
  writes, then brings both back: a short dip in the sound and a blink of the key lights in place of a click and a
  flash;
- opens NVS read-write only for the write, as Net's handover does: `nvs_flash_init_partition` when nobody has it
  open (a firmware that has it open keeps it), `nvs_flash_deinit_partition` after; never an erase: a partition that
  would need one (`ESP_ERR_NVS_NO_FREE_PAGES`, `ESP_ERR_NVS_NEW_VERSION_FOUND`) is the Launcher's to repair, and the
  settings are kept for this run only;
- reads at the start without the NVS library and without writing (the partition through the cache, page by page,
  as the system menu has always read the Launcher's values), so a start never writes the flash.

The write and the HAL's other NVS writer (Net's handover, `l_pending`) and reader take one lock
(`System::detail::NvsWriteMutex`), since each may open and close the partition.

## Over USB

The settings are part of the HAL's system channel in every firmware that starts USB, also when the development
channel is off (`CONFIG_CARDENZA_HAL_USB_DEV`): `SETTINGS`, `SETTING_GET`, `SETTING_SET` and `SETTINGS_RESET` in
[usb-dev.md](usb-dev.md); `203cli board settings` on the computer.
