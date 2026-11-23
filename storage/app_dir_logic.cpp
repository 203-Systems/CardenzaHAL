#include "app_dir_logic.h"

#include <cctype>

namespace CardenzaHAL::Storage::AppDirLogic {

namespace {

constexpr std::size_t kMaxFolderName = 31;  // an app description's project name holds 31 characters

bool Kept(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
}

bool Trimmed(char c)
{
    return c == '.' || c == '_' || c == '-';
}

bool ReservedOnWindows(const std::string& name)
{
    // Windows refuses these names with any extension: a card read on a PC must not lose the folder.
    std::string stem = name.substr(0, name.find('.'));
    for (char& c : stem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return true;
    return stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) && stem[3] >= '1' && stem[3] <= '9';
}

void Note(std::vector<std::string>& names, const std::string& name)
{
    if (names.size() < MigrateReport::kMaxNames) names.push_back(name);
}

void Refused(MigrateReport& report, const std::string& name)
{
    ++report.failed;
    Note(report.failedNames, name.empty() ? "." : name);
}

std::string Parent(const std::string& path)
{
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos || slash == 0 ? std::string() : path.substr(0, slash);
}

// `relative` is where `oldDir` is below the folder the migration started from ("" for that folder).
void MergeInto(FileSystem& fs, const std::string& oldDir, const std::string& newDir, const std::string& relative,
               MigrateReport& report)
{
    std::vector<std::string> names;
    if (!fs.List(oldDir, names))
    {
        Refused(report, relative);
        return;
    }
    for (const std::string& name : names)
    {
        const std::string from = JoinPath(oldDir, name);
        const std::string to = JoinPath(newDir, name);
        const std::string inside = JoinPath(relative, name);
        // FAT compares names without case: Stat finds "settings" for "Settings", so that is the same entry.
        const FileSystem::Kind there = fs.Stat(to);
        if (there == FileSystem::Kind::None)
        {
            if (fs.Rename(from, to) || (fs.Stat(from) == FileSystem::Kind::None && fs.Stat(to) != FileSystem::Kind::None))
                ++report.moved;
            else
                Refused(report, inside);
        }
        else if (there == FileSystem::Kind::Directory && fs.Stat(from) == FileSystem::Kind::Directory)
        {
            MergeInto(fs, from, to, inside, report);
        }
        else
        {
            ++report.left;
            Note(report.leftNames, inside);
        }
    }
    names.clear();
    if (fs.List(oldDir, names) && names.empty() && !fs.RemoveDirectory(oldDir)) Refused(report, relative);
}

}  // namespace

std::string SafeFolderName(std::string_view name)
{
    std::string safe;
    for (const char c : name)
    {
        const char kept = Kept(c) ? c : '_';
        if (kept == '_' && !safe.empty() && safe.back() == '_' && !Kept(c)) continue;  // one '_' for a run
        safe.push_back(kept);
    }
    std::size_t first = 0;
    while (first < safe.size() && Trimmed(safe[first])) ++first;
    safe.erase(0, first);
    if (safe.size() > kMaxFolderName) safe.resize(kMaxFolderName);
    while (!safe.empty() && Trimmed(safe.back())) safe.pop_back();
    if (!safe.empty() && ReservedOnWindows(safe)) safe.push_back('_');
    return safe;
}

std::string JoinPath(std::string_view directory, std::string_view name)
{
    while (!name.empty() && name.front() == '/') name.remove_prefix(1);
    std::string path(directory);
    if (name.empty()) return path;
    if (path.empty()) return std::string(name);
    if (path.back() != '/') path.push_back('/');
    path.append(name);
    return path;
}

bool MakeDirectories(FileSystem& fs, const std::string& root, std::string_view relative)
{
    std::string path = root;
    while (!relative.empty())
    {
        const std::size_t slash = relative.find('/');
        const std::string_view part = relative.substr(0, slash);
        relative = slash == std::string_view::npos ? std::string_view() : relative.substr(slash + 1);
        if (part.empty() || part == ".") continue;
        path = JoinPath(path, part);
        const FileSystem::Kind kind = fs.Stat(path);
        if (kind == FileSystem::Kind::Directory) continue;
        if (kind == FileSystem::Kind::File) return false;
        // Another task may have made it meanwhile: what counts is that it is there.
        if (!fs.MakeDirectory(path) && fs.Stat(path) != FileSystem::Kind::Directory) return false;
    }
    return fs.Stat(path) == FileSystem::Kind::Directory;
}

MigrateReport Migrate(FileSystem& fs, const std::string& root, const std::string& oldDir, const std::string& newDir)
{
    MigrateReport report;
    if (oldDir.empty() || newDir.empty() || oldDir == newDir || fs.Stat(oldDir) != FileSystem::Kind::Directory) return report;

    const FileSystem::Kind there = fs.Stat(newDir);
    if (there == FileSystem::Kind::File)
    {
        report.outcome = MigrateReport::Outcome::Failed;
        Refused(report, newDir);
        return report;
    }
    if (there == FileSystem::Kind::None)
    {
        // The new folder's parents (/sdcard/Apps): a power cut after this leaves an empty folder, nothing else.
        const std::string parent = Parent(newDir);
        const bool belowRoot = parent.size() > root.size() && parent.compare(0, root.size(), root) == 0 && parent[root.size()] == '/';
        if (belowRoot && !MakeDirectories(fs, root, std::string_view(parent).substr(root.size() + 1)))
        {
            report.outcome = MigrateReport::Outcome::Failed;
            Refused(report, parent);
            return report;
        }
        // One rename: the folder and everything in it change place at once, nothing is copied. A rename that
        // reports a failure but happened anyway counts as done.
        if (fs.Rename(oldDir, newDir) ||
            (fs.Stat(oldDir) == FileSystem::Kind::None && fs.Stat(newDir) == FileSystem::Kind::Directory))
        {
            report.outcome = MigrateReport::Outcome::Renamed;
            report.moved = 1;
        }
        else
        {
            report.outcome = MigrateReport::Outcome::Failed;
            Refused(report, "");
        }
        return report;
    }

    // Both are there (a later start after a downgrade, or a move cut short): merge, never overwrite.
    MergeInto(fs, oldDir, newDir, "", report);
    report.outcome = report.failed > 0 ? MigrateReport::Outcome::Failed : MigrateReport::Outcome::Merged;
    return report;
}

std::string Describe(const MigrateReport& report, const std::string& oldDir, const std::string& newDir)
{
    const auto list = [](const std::vector<std::string>& names, int count) {
        std::string text;
        for (const std::string& name : names) text += (text.empty() ? "" : ", ") + name;
        if (count > static_cast<int>(names.size())) text += ", ...";
        return text;
    };
    std::string line;
    switch (report.outcome)
    {
    case MigrateReport::Outcome::Nothing:
        return line;
    case MigrateReport::Outcome::NoCard:
        return "no card: " + oldDir + " not looked at";
    case MigrateReport::Outcome::Renamed:
        return "moved " + oldDir + " to " + newDir;
    case MigrateReport::Outcome::Merged:
        line = "merged " + oldDir + " into " + newDir + ": " + std::to_string(report.moved) + " moved";
        break;
    case MigrateReport::Outcome::Failed:
        line = "moving " + oldDir + " to " + newDir + " stopped: " + std::to_string(report.moved) + " moved, " +
               std::to_string(report.failed) + " refused (" + list(report.failedNames, report.failed) + "), again at the next start";
        break;
    }
    if (report.left > 0)
        line += ", " + std::to_string(report.left) + " left in the old folder (already there: " + list(report.leftNames, report.left) + ")";
    else if (report.outcome == MigrateReport::Outcome::Merged)
        line += ", the old folder removed";
    return line;
}

}  // namespace CardenzaHAL::Storage::AppDirLogic
