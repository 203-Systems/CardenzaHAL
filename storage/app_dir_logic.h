#pragma once

// The pure part of storage/app_dir.h (no ESP-IDF; host tests and simulators may use it): a firmware's folder
// name, and moving a folder to a new place on the card so that a power cut at any moment loses nothing.
// The SD card's layout is docs/SD-CARD.md.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace CardenzaHAL::Storage {

// What MigrateAppDir did (storage/app_dir.h). Entries are files or folders, named relative to the old folder.
struct MigrateReport {
    enum class Outcome {
        Nothing,  // no old folder (or it is the new one): nothing to do; costs a stat
        Renamed,  // the old folder became the new one: one rename, nothing copied
        Merged,   // both were there: what the new one lacked moved in, the rest stayed (left)
        Failed,   // the card refused a step (or a file is where the new folder goes): run it again at the next start
        NoCard,   // no card mounted (MigrateAppDir only)
    };
    Outcome outcome = Outcome::Nothing;
    int moved = 0;   // renames into the new folder (a whole folder counts once)
    int left = 0;    // entries left in the old folder because the new one has the same name: never overwritten
    int failed = 0;  // renames, folders made or listed that the card refused
    std::vector<std::string> leftNames;    // the first kMaxNames of them
    std::vector<std::string> failedNames;  // the first kMaxNames of them
    static constexpr std::size_t kMaxNames = 4;
};

namespace AppDirLogic {

// The folder name of a firmware: ASCII letters, digits, '-', '_' and '.' (every other character, a space or
// a UTF-8 byte, becomes '_', a run of them one '_'); no '.', '_' or '-' at either end; at most 31 characters;
// a name Windows reserves (CON, NUL, COM1, ...) gets a '_' after it. Empty when nothing is left of `name`.
std::string SafeFolderName(std::string_view name);

// "a" + "/" + "b" without doubling the '/'; an empty part gives the other.
std::string JoinPath(std::string_view directory, std::string_view name);

// The few file-system calls the migration makes. Each is one operation on the card: the migration is only
// renames, folders made and empty folders removed, so the data is always in the old place or the new one.
class FileSystem {
public:
    enum class Kind { None, File, Directory };
    virtual ~FileSystem() = default;
    virtual Kind Stat(const std::string& path) = 0;
    // The names in a folder, without "." and "..". False when it cannot be read.
    virtual bool List(const std::string& path, std::vector<std::string>& names) = 0;
    // One rename on the same volume (a folder with everything in it). Never over an existing name.
    virtual bool Rename(const std::string& from, const std::string& to) = 0;
    // One folder; its parent exists.
    virtual bool MakeDirectory(const std::string& path) = 0;
    // An empty folder only.
    virtual bool RemoveDirectory(const std::string& path) = 0;
};

// mkdir -p of `root`/`relative`, from the first missing folder below `root` (a mount point, never made).
// True when the folder is there.
bool MakeDirectories(FileSystem& fs, const std::string& root, std::string_view relative);

// Moves `oldDir` to `newDir` (two folders on one volume; `root` is the mount point, under which `newDir`'s
// parents are made when missing):
// - no old folder, or the old one is the new one: Nothing;
// - old there, new missing: its parents are made and the old folder is renamed to the new one (Renamed);
// - both there: each entry of the old folder whose name the new one lacks is renamed into it, a folder that
//   both have is merged the same way, and an entry the new one has already is left where it is (never
//   overwritten, never deleted); a folder emptied that way is removed (Merged);
// - a file where the new folder goes, or a step the card refuses: Failed (what was done stays done).
// A power cut at any point leaves every entry in the old place or the new one; running it again finishes
// the move, and once done it costs a stat.
MigrateReport Migrate(FileSystem& fs, const std::string& root, const std::string& oldDir, const std::string& newDir);

// The log line of a migration ("" for Nothing), e.g. "moved /sdcard/Synth to /sdcard/Apps/Synth" or
// "merged /sdcard/Synth into /sdcard/Apps/Synth: 3 moved, 2 left in the old folder (already there:
// Settings/a.cfg, Session/x.txt)".
std::string Describe(const MigrateReport& report, const std::string& oldDir, const std::string& newDir);

}  // namespace AppDirLogic
}  // namespace CardenzaHAL::Storage
