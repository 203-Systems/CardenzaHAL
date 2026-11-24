# The SD card's layout

[中文](SD-CARD.zh-CN.md)

Every firmware on a Cardenza shares one SD card with the 203 Launcher and with each other (the user's
decision, 2026-11-23). Nothing is kept at the card's root: the Launcher has one folder, and every firmware
one of its own.

```
/sdcard/                      the card (FAT, mounted by CardenzaHAL::Init: storage/storage.h)
  Launcher/                   the 203 Launcher's: its downloads, its settings file, backups, the font pack
  Apps/
    <AppName>/                one firmware's (storage/app_dir.h: Storage::AppDir())
      Settings/               its settings files
      Logs/                   its logs
      ...                     the rest is the firmware's own (projects, presets, samples, saves, caches)
    CardenzaSynth/            for example Cardenza Synth's
```

## Rules for a firmware

- **Everything in your folder**, `/sdcard/Apps/<AppName>/`: `Storage::AppDir()`, `Storage::AppPath("Settings/app.cfg")`.
  Make a folder before writing in it with `Storage::EnsureAppDir("Settings")` (it makes `/sdcard/Apps/<AppName>` too).
  Never write at the card's root, in `/sdcard/Launcher/` or in another firmware's folder; reading another
  firmware's files is fine when the user asks for it (a sample, a project), writing them is not.
- **`Settings/` and `Logs/`** are where people (and tools such as `203cli board files`) look for a firmware's
  settings and logs. Name the rest as you like.
- **The board's settings are not files**: the volume, the screen's brightness, the key lights and the language are
  kept in flash by the HAL, the same in every firmware (`system/settings.h`,
  [SETTINGS.md](SETTINGS.md)). Do not keep copies of them on the card.
- **The folder's name** (`Storage::AppName()`) is `CONFIG_CARDENZA_HAL_APP_NAME` (menuconfig: CardenzaHAL) when it is
  set, else the app description's project name (CMake's `project()`, which `203cli new` sets), made a safe FAT
  folder name: ASCII letters, digits, `-`, `_` and `.`, any other character `_` (a run of them one), none of `.`,
  `_`, `-` at either end, at most 31 characters, a name Windows reserves (`CON`, `NUL`, `COM1`, ...) with a `_`
  after it; `App` when nothing is left. Choose it once: renaming it later means moving your users' files
  (below).
- **No card is normal**: run without one (`Storage::IsMounted()` false); never wait for a card, format one or
  restart because of it.
- **The power goes whenever the user pulls it.** Write a file under a temporary name and rename it into place
  (FAT does not rename over a file: remove the old one first, and read the temporary one when only it is
  there); the template's `WriteFile` / `ReadFile` do that.
- File names: FAT long names, ASCII for anything a firmware makes (ESP-IDF's FAT is built for one code page),
  `/` between folders, no `\ : * ? " < > |`.

The template (`examples/template`, what `203cli new` copies) does all of this: it counts its starts in
`Settings/app.cfg` and logs a line a start to `Logs/log.txt`.

## Moving old files

A firmware that kept its files elsewhere before (its own folder at the card's root, as Cardenza Synth's
`/sdcard/CardenzaSynth/` until 2026-11) moves them once, at the start, before anything reads them:

```cpp
CardenzaHAL::Init();
CardenzaHAL::Storage::MigrateAppDir("/sdcard/MyFirmware");  // then read Settings/... from AppDir()
```

What it does (`storage/app_dir_logic.h`, tested on a fake card that loses power before each step):

| On the card | What happens |
| --- | --- |
| no old folder | nothing (one `stat`): keep the call for as long as old cards may come |
| only the old folder | one rename to `/sdcard/Apps/<AppName>` (`/sdcard/Apps` made first): nothing is copied, it takes milliseconds whatever is in it |
| both folders | merged: each entry the new folder lacks is renamed into it; a folder both have is merged the same way; an entry both have stays in the old folder, never overwritten and never deleted; a folder that ends up empty is removed |
| a file where the new folder goes, or the card refuses a step | it stops there (what moved stays moved) and tries again at the next start |

- **A power cut at any moment loses nothing.** Every step is one rename, one folder made or one empty folder
  removed, so each file is in the old place or the new one; the next start runs the same steps and finishes.
  (FAT's rename writes the new name before it clears the old one: cut between the two, both names show the
  same files, which the next start leaves alone; a computer's disk check tidies that up.)
- **One log line** (tag `CardenzaStorage`) when something moved or was left: `moved /sdcard/MyFirmware to
  /sdcard/Apps/MyFirmware`, or `merged ...: 3 moved, 2 left in the old folder (already there: Settings/app.cfg,
  ...)`. What was left stays in the old folder until the user removes it (or, once the new folder lacks it,
  the next start moves it in).
- It needs a mounted card and takes the card for its own: call it before anything else uses files.

### Going back to an older version

A user may install an older version of the firmware after the move. That version still looks in its old
folder: it finds it gone (or holding only what the merge left), starts as on a new card (its default settings,
none of the user's files) and makes the old folder again for what it saves. Nothing in `/sdcard/Apps/` is
touched by it. When the user installs the newer version again, the merge brings across what the older one
saved there that the new folder lacks; what both have (its settings file, say, or a working file it rewrote)
stays in the old folder: copy it over by hand to keep that version of it. Say this in your release notes when
you move.

## The Launcher's folder

`/sdcard/Launcher/` is the 203 Launcher's (the firmware it downloads, its settings file, backups of
partitions, the font pack). A firmware reads nothing there and writes nothing there; the Launcher moved its
own files there from the card's root itself (203-launcher).

## From a computer

`203cli board files` reads and writes the card of a Cardenza on USB while a firmware on CardenzaHAL runs
(`docs/usb-dev.md`): `203cli board files ls Apps/<AppName>`, `203cli board files get Apps/<AppName>/Logs/log.txt`.
A card in a computer's reader shows the same folders.

The web build (`tools/cardenza_web.py`) has a card at `/sdcard` too, kept in the browser: the same paths work.
