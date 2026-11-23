#pragma once

// A firmware's own folder on the SD card (docs/SD-CARD.md, the user's decision 2026-11-23): every firmware on
// CardenzaHAL keeps its files in /sdcard/Apps/<AppName>/, and the 203 Launcher keeps its own in
// /sdcard/Launcher/. Nothing else goes at the card's root.
//
//   /sdcard/Launcher/             the 203 Launcher's (downloads, its card settings, backups): not a firmware's
//   /sdcard/Apps/<AppName>/       this firmware's
//       Settings/                 its settings files (the board's own settings are system/settings.h, in flash)
//       Logs/                     its logs
//       ...                       the rest is the firmware's own (projects, presets, saves, caches)
//
// The card is mounted by CardenzaHAL::Init (storage/storage.h); these need a mounted card, except AppName,
// AppDir and AppPath, which only compute paths. Any task; nothing here allocates on the card's DMA.

#include <string>
#include <string_view>

#include "app_dir_logic.h"
#include "storage.h"

namespace CardenzaHAL::Storage {

    inline constexpr char kAppsDir[] = "/sdcard/Apps";          // one folder per firmware
    inline constexpr char kLauncherDir[] = "/sdcard/Launcher";  // the 203 Launcher's: never write there
    inline constexpr char kSettingsFolder[] = "Settings";       // the conventional sub-folders of AppDir()
    inline constexpr char kLogsFolder[] = "Logs";

    // This firmware's folder name: CONFIG_CARDENZA_HAL_APP_NAME (menuconfig: CardenzaHAL) when it is set,
    // else the app description's project name (CMake's project()), made a safe FAT folder name
    // (AppDirLogic::SafeFolderName: ASCII letters, digits, '-', '_', '.'). "App" when nothing is left of it.
    std::string AppName();
    // "/sdcard/Apps/<AppName>".
    std::string AppDir();
    // A path in it: AppPath("Settings/app.cfg") is "/sdcard/Apps/<AppName>/Settings/app.cfg".
    std::string AppPath(std::string_view relative);

    // Makes AppDir() (and its parent /sdcard/Apps), and `subfolder` in it when given ("Settings", "Logs",
    // "Projects/Old", ...). True when the folder is there; false without a mounted card or when the card
    // refuses. A firmware calls it before writing a file in a folder that may not exist yet.
    bool EnsureAppDir(std::string_view subfolder = {});

    // For a firmware that kept its files elsewhere before (its own folder at the card's root, say
    // "/sdcard/MyFirmware"): moves them to AppDir(), once, at the start, before anything reads them.
    // - Only the old folder there: one rename (the same card: nothing is copied, it takes milliseconds).
    // - Both there (a start after the user went back to an older version and forward again): what AppDir()
    //   lacks moves in, a folder both have is merged, and an entry AppDir() has already stays in the old
    //   folder (never overwritten, never deleted); a folder emptied that way is removed.
    // - Safe against a power cut at any moment: each step is one rename, one folder made or one empty folder
    //   removed, so every file is in the old place or the new one; calling it again at the next start finishes.
    // `oldDir` is a path on the card: "/sdcard/MyFirmware", or "MyFirmware" / "/MyFirmware" from the card's
    // root. It logs one line (tag CardenzaStorage) when it moved or left something, and costs a stat when
    // there is nothing to move: keep the call in the firmware for as long as old cards may come.
    MigrateReport MigrateAppDir(std::string_view oldDir);
}
