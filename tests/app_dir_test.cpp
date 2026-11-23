// storage/app_dir_logic.h: a firmware's folder name, and moving its old folder to /sdcard/Apps/<AppName> on a
// fake FAT card (names without case, no rename over a name, an empty folder only removed) that can lose power
// before any step: every file is always in the old place or the new one, and the next start finishes the move.
#include <cassert>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "storage/app_dir_logic.h"

using namespace CardenzaHAL::Storage;
using AppDirLogic::FileSystem;

namespace {

struct PowerCut {};

std::string Lower(std::string text)
{
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string ParentOf(const std::string& path)
{
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// A FAT card at /sdcard: paths compare without case; what each entry is called is what it was made as.
class FakeCard final : public FileSystem {
public:
    struct Node {
        std::string path;  // as made
        bool directory = false;
        std::string contents;
    };

    // Mutating calls left before the power goes (-1: never).
    int budget = -1;
    int mutations = 0;
    // A rename that does its work and reports a failure.
    bool renameLies = false;

    FakeCard() { nodes_["/sdcard"] = {"/sdcard", true, {}}; }

    void File(const std::string& path, const std::string& contents)
    {
        Folders(ParentOf(path));
        nodes_[Lower(path)] = {path, false, contents};
    }
    void Folders(const std::string& path)
    {
        if (path.empty() || nodes_.count(Lower(path))) return;
        Folders(ParentOf(path));
        nodes_[Lower(path)] = {path, true, {}};
    }

    // Every file below `root` by its path relative to it, and its contents.
    std::map<std::string, std::string> Files(const std::string& root) const
    {
        std::map<std::string, std::string> files;
        const std::string prefix = Lower(root) + "/";
        for (const auto& [key, node] : nodes_)
            if (!node.directory && key.compare(0, prefix.size(), prefix) == 0) files[Lower(node.path.substr(prefix.size()))] = node.contents;
        return files;
    }
    std::set<std::string> Paths() const
    {
        std::set<std::string> paths;
        for (const auto& [key, node] : nodes_) paths.insert(node.path + (node.directory ? "/" : "=" + node.contents));
        return paths;
    }
    bool Has(const std::string& path) const { return nodes_.count(Lower(path)) != 0; }
    std::string Contents(const std::string& path) const { return nodes_.at(Lower(path)).contents; }

    Kind Stat(const std::string& path) override
    {
        const auto it = nodes_.find(Lower(path));
        if (it == nodes_.end()) return Kind::None;
        return it->second.directory ? Kind::Directory : Kind::File;
    }

    bool List(const std::string& path, std::vector<std::string>& names) override
    {
        if (Stat(path) != Kind::Directory) return false;
        for (const auto& [key, node] : nodes_)
            if (key != Lower(path) && Lower(ParentOf(node.path)) == Lower(path)) names.push_back(node.path.substr(node.path.find_last_of('/') + 1));
        return true;
    }

    bool Rename(const std::string& from, const std::string& to) override
    {
        Step();
        if (Stat(from) == Kind::None || Stat(to) != Kind::None || Stat(ParentOf(to)) != Kind::Directory) return false;
        const std::string prefix = Lower(from);
        std::map<std::string, Node> moved;
        for (auto it = nodes_.begin(); it != nodes_.end();)
        {
            if (it->first == prefix || it->first.compare(0, prefix.size() + 1, prefix + "/") == 0)
            {
                Node node = it->second;
                node.path = to + node.path.substr(from.size());
                moved[Lower(node.path)] = node;
                it = nodes_.erase(it);
            }
            else ++it;
        }
        nodes_.insert(moved.begin(), moved.end());
        return !renameLies;
    }

    bool MakeDirectory(const std::string& path) override
    {
        Step();
        if (Stat(path) != Kind::None || Stat(ParentOf(path)) != Kind::Directory) return false;
        nodes_[Lower(path)] = {path, true, {}};
        return true;
    }

    bool RemoveDirectory(const std::string& path) override
    {
        Step();
        std::vector<std::string> names;
        if (!List(path, names) || !names.empty()) return false;
        nodes_.erase(Lower(path));
        return true;
    }

private:
    void Step()
    {
        if (budget == 0) throw PowerCut{};
        if (budget > 0) --budget;
        ++mutations;
    }

    std::map<std::string, Node> nodes_;
};

const std::string kRoot = "/sdcard";
const std::string kOld = "/sdcard/CardenzaSynth";
const std::string kNew = "/sdcard/Apps/CardenzaSynth";

void TestSafeFolderName()
{
    using AppDirLogic::SafeFolderName;
    assert(SafeFolderName("CardenzaSynth") == "CardenzaSynth");
    assert(SafeFolderName("my_firmware") == "my_firmware");
    assert(SafeFolderName("Cardenza Synth") == "Cardenza_Synth");
    assert(SafeFolderName("a  /\\:*?\"<>|b") == "a_b");
    assert(SafeFolderName("a__b") == "a__b");
    assert(SafeFolderName("..hidden..") == "hidden");
    assert(SafeFolderName("-x-") == "x");
    assert(SafeFolderName("v1.2") == "v1.2");
    assert(SafeFolderName("鸣片") == "");
    assert(SafeFolderName("鸣片 Synth") == "Synth");
    assert(SafeFolderName("") == "" && SafeFolderName("...") == "");
    assert(SafeFolderName("con") == "con_" && SafeFolderName("NUL") == "NUL_" && SafeFolderName("COM3.x") == "COM3.x_");
    assert(SafeFolderName("COM0") == "COM0" && SafeFolderName("CONSOLE") == "CONSOLE");
    const std::string longName = SafeFolderName(std::string(40, 'a'));
    assert(longName == std::string(31, 'a'));
    assert(SafeFolderName(std::string(30, 'a') + "_bc") == std::string(30, 'a'));  // no '_' left at the end
}

void TestJoinAndMake()
{
    using AppDirLogic::JoinPath;
    assert(JoinPath("/sdcard", "Apps") == "/sdcard/Apps" && JoinPath("/sdcard/", "/Apps") == "/sdcard/Apps");
    assert(JoinPath("", "x") == "x" && JoinPath("/a", "") == "/a");

    FakeCard card;
    assert(AppDirLogic::MakeDirectories(card, kRoot, "Apps/CardenzaSynth/Settings"));
    assert(card.Has("/sdcard/Apps/CardenzaSynth/Settings") && card.mutations == 3);
    assert(AppDirLogic::MakeDirectories(card, kRoot, "Apps/CardenzaSynth/Settings") && card.mutations == 3);  // nothing to do
    assert(AppDirLogic::MakeDirectories(card, kRoot, "apps//cardenzasynth/./Logs") && card.Has("/sdcard/Apps/CardenzaSynth/Logs"));
    card.File("/sdcard/Apps/Blocked", "a file");
    assert(!AppDirLogic::MakeDirectories(card, kRoot, "Apps/Blocked/Settings"));
}

// The 0.1.0 release's folder, as a card of a user has it.
void OldFolder(FakeCard& card)
{
    card.File(kOld + "/Settings/system.cfg", "sys.theme=dark");
    card.File(kOld + "/Settings/synth.cfg", "keys.select=one");
    card.File(kOld + "/Projects/DEMO-2.ccproj", "CARDENZA_PROJECT 6");
    card.File(kOld + "/Projects/SONG.ccproj", "CARDENZA_PROJECT 7");
    card.File(kOld + "/Presets/Juno/PAD.ccpreset", "juno");
    card.File(kOld + "/Samples/kick.wav", "RIFF1");
    card.File(kOld + "/Session/autosave.ccproj", "CARDENZA_PROJECT 7 old");
    card.File(kOld + "/Project/OLD.txt", "before 0.1.0");  // a layout nobody reads: moves with its folder, unchanged
    card.Folders(kOld + "/Logs");
}

void TestNothingToDo()
{
    FakeCard card;
    MigrateReport report = AppDirLogic::Migrate(card, kRoot, kOld, kNew);
    assert(report.outcome == MigrateReport::Outcome::Nothing && card.mutations == 0);
    assert(AppDirLogic::Describe(report, kOld, kNew).empty());
    card.File(kOld, "a file, not a folder");
    report = AppDirLogic::Migrate(card, kRoot, kOld, kNew);
    assert(report.outcome == MigrateReport::Outcome::Nothing && card.mutations == 0);
    report = AppDirLogic::Migrate(card, kRoot, kNew, kNew);
    assert(report.outcome == MigrateReport::Outcome::Nothing);
}

void TestRename()
{
    FakeCard card;
    OldFolder(card);
    const auto before = card.Files(kOld);
    const MigrateReport report = AppDirLogic::Migrate(card, kRoot, kOld, kNew);
    assert(report.outcome == MigrateReport::Outcome::Renamed && report.moved == 1 && report.left == 0 && report.failed == 0);
    // /sdcard/Apps made, then one rename: two steps.
    assert(card.mutations == 2);
    assert(!card.Has(kOld) && card.Files(kNew) == before && card.Has(kNew + "/Logs"));
    assert(AppDirLogic::Describe(report, kOld, kNew) == "moved /sdcard/CardenzaSynth to /sdcard/Apps/CardenzaSynth");
    // Done: the next start costs a stat.
    card.mutations = 0;
    assert(AppDirLogic::Migrate(card, kRoot, kOld, kNew).outcome == MigrateReport::Outcome::Nothing && card.mutations == 0);
}

void TestRenameThatReportsFailure()
{
    FakeCard card;
    OldFolder(card);
    card.renameLies = true;
    const MigrateReport report = AppDirLogic::Migrate(card, kRoot, kOld, kNew);
    assert(report.outcome == MigrateReport::Outcome::Renamed && !card.Has(kOld) && card.Has(kNew + "/Projects/SONG.ccproj"));
}

// After a downgrade: the older firmware made its folder again and saved a project there; the new folder has
// what the newer firmware kept.
void BothFolders(FakeCard& card)
{
    card.File(kNew + "/Settings/system.cfg", "sys.theme=light");
    card.File(kNew + "/Projects/DEMO-2.ccproj", "CARDENZA_PROJECT 7 new");
    card.File(kNew + "/Session/autosave.ccproj", "CARDENZA_PROJECT 7 new");
    card.Folders(kNew + "/Samples");
    card.File(kOld + "/Settings/system.cfg", "sys.theme=retro");
    card.File(kOld + "/settings/synth.cfg", "keys.select=multi");  // FAT: the same folder as Settings
    card.File(kOld + "/Projects/DEMO-2.ccproj", "CARDENZA_PROJECT 7 downgrade");
    card.File(kOld + "/Projects/NEW.ccproj", "CARDENZA_PROJECT 7 made while downgraded");
    card.File(kOld + "/Samples/snare.wav", "RIFF2");
    card.File(kOld + "/Presets/DX7/BRASS.ccpreset", "dx7");
    card.Folders(kOld + "/Session");
}

void TestMerge()
{
    FakeCard card;
    BothFolders(card);
    const MigrateReport report = AppDirLogic::Migrate(card, kRoot, kOld, kNew);
    assert(report.outcome == MigrateReport::Outcome::Merged);
    // Moved: Settings/synth.cfg, Projects/NEW.ccproj, Samples/snare.wav, Presets (a whole folder).
    assert(report.moved == 4 && report.left == 2 && report.failed == 0);
    assert(report.leftNames == (std::vector<std::string>{"Projects/DEMO-2.ccproj", "Settings/system.cfg"}));
    // Nothing overwritten: the new folder keeps its own; the old one keeps the two it could not move.
    assert(card.Contents(kNew + "/Settings/system.cfg") == "sys.theme=light");
    assert(card.Contents(kNew + "/Projects/DEMO-2.ccproj") == "CARDENZA_PROJECT 7 new");
    assert(card.Contents(kNew + "/Settings/synth.cfg") == "keys.select=multi");
    assert(card.Contents(kNew + "/Projects/NEW.ccproj") == "CARDENZA_PROJECT 7 made while downgraded");
    assert(card.Contents(kNew + "/Presets/DX7/BRASS.ccpreset") == "dx7" && card.Has(kNew + "/Samples/snare.wav"));
    assert(card.Files(kOld) == (std::map<std::string, std::string>{{"projects/demo-2.ccproj", "CARDENZA_PROJECT 7 downgrade"},
                                                                  {"settings/system.cfg", "sys.theme=retro"}}));
    // Emptied folders went (Samples, Session); the ones with a file left stay.
    assert(!card.Has(kOld + "/Samples") && !card.Has(kOld + "/Session") && card.Has(kOld + "/Projects"));
    assert(AppDirLogic::Describe(report, kOld, kNew) ==
           "merged /sdcard/CardenzaSynth into /sdcard/Apps/CardenzaSynth: 4 moved, 2 left in the old folder (already there: "
           "Projects/DEMO-2.ccproj, Settings/system.cfg)");
    // The next start moves nothing and changes nothing.
    card.mutations = 0;
    const MigrateReport again = AppDirLogic::Migrate(card, kRoot, kOld, kNew);
    assert(again.outcome == MigrateReport::Outcome::Merged && again.moved == 0 && again.left == 2 && card.mutations == 0);
    // Once the user takes the leftovers away, the old folder goes.
    card = FakeCard();
    card.File(kNew + "/Settings/system.cfg", "x");
    card.File(kOld + "/Projects/A.ccproj", "a");
    card.File(kOld + "/Projects/B.ccproj", "b");
    const MigrateReport clean = AppDirLogic::Migrate(card, kRoot, kOld, kNew);
    assert(clean.outcome == MigrateReport::Outcome::Merged && clean.moved == 1 && clean.left == 0 && !card.Has(kOld));
    assert(AppDirLogic::Describe(clean, kOld, kNew) ==
           "merged /sdcard/CardenzaSynth into /sdcard/Apps/CardenzaSynth: 1 moved, the old folder removed");
}

void TestConflictsOfKind()
{
    FakeCard card;
    card.File(kNew + "/Logs", "a file where the old folder has a folder");
    card.File(kOld + "/Logs/log.txt", "boot");
    card.File(kNew + "/Notes/a.txt", "a");
    card.File(kOld + "/Notes", "a file where the new folder has a folder");
    const MigrateReport report = AppDirLogic::Migrate(card, kRoot, kOld, kNew);
    assert(report.outcome == MigrateReport::Outcome::Merged && report.moved == 0 && report.left == 2);
    assert(card.Contents(kOld + "/Logs/log.txt") == "boot" && card.Contents(kOld + "/Notes") == "a file where the new folder has a folder");
    // More names than the report keeps.
    card = FakeCard();
    for (int i = 0; i < 6; ++i)
    {
        card.File(kNew + "/f" + std::to_string(i), "new");
        card.File(kOld + "/f" + std::to_string(i), "old");
    }
    const MigrateReport many = AppDirLogic::Migrate(card, kRoot, kOld, kNew);
    assert(many.left == 6 && many.leftNames.size() == MigrateReport::kMaxNames);
    assert(AppDirLogic::Describe(many, kOld, kNew).find("f0, f1, f2, f3, ...)") != std::string::npos);
}

void TestBlocked()
{
    // A file where the new folder goes: nothing moves, and the log says so.
    FakeCard card;
    OldFolder(card);
    card.File(kNew, "a file");
    MigrateReport report = AppDirLogic::Migrate(card, kRoot, kOld, kNew);
    assert(report.outcome == MigrateReport::Outcome::Failed && report.moved == 0 && card.mutations == 0);
    assert(AppDirLogic::Describe(report, kOld, kNew).find("stopped") != std::string::npos);
    // A file where /sdcard/Apps goes.
    card = FakeCard();
    OldFolder(card);
    card.File("/sdcard/Apps", "a file");
    report = AppDirLogic::Migrate(card, kRoot, kOld, kNew);
    assert(report.outcome == MigrateReport::Outcome::Failed && card.Has(kOld + "/Projects/SONG.ccproj"));
}

// Every file of `expected` (relative to the old folder or the new one) is in one of the two places, unchanged.
void CheckNothingLost(const FakeCard& card, const std::map<std::string, std::string>& oldFiles,
                      const std::map<std::string, std::string>& newFiles)
{
    const auto inOld = card.Files(kOld);
    const auto inNew = card.Files(kNew);
    for (const auto& [name, contents] : oldFiles)
    {
        const auto o = inOld.find(name);
        const auto n = inNew.find(name);
        const bool keptOld = o != inOld.end() && o->second == contents;
        const bool movedNew = n != inNew.end() && n->second == contents;
        assert(keptOld || movedNew);
    }
    for (const auto& [name, contents] : newFiles)
    {
        const auto n = inNew.find(name);
        assert(n != inNew.end() && n->second == contents);  // never overwritten
    }
}

// The power goes before each step in turn; the next start runs the migration again and ends where an
// uninterrupted one ends.
template <typename Setup>
void TestPowerCutAtEachStep(Setup setup)
{
    FakeCard whole;
    setup(whole);
    AppDirLogic::Migrate(whole, kRoot, kOld, kNew);
    const int steps = whole.mutations;
    const std::set<std::string> finished = whole.Paths();
    assert(steps > 0);
    for (int cut = 0; cut < steps; ++cut)
    {
        FakeCard card;
        setup(card);
        const auto oldFiles = card.Files(kOld);
        const auto newFiles = card.Files(kNew);
        card.budget = cut;
        bool lost = false;
        try
        {
            AppDirLogic::Migrate(card, kRoot, kOld, kNew);
        }
        catch (const PowerCut&)
        {
            lost = true;
        }
        assert(lost);
        CheckNothingLost(card, oldFiles, newFiles);
        card.budget = -1;
        AppDirLogic::Migrate(card, kRoot, kOld, kNew);  // the next start
        CheckNothingLost(card, oldFiles, newFiles);
        assert(card.Paths() == finished);
    }
}

// FAT's rename writes the new name before it clears the old one: cut between the two, both names hold the
// same files. The next start moves nothing over them and deletes neither.
void TestCutInsideARename()
{
    FakeCard card;
    OldFolder(card);
    const auto files = card.Files(kOld);
    for (const auto& [name, contents] : files) card.File(kNew + "/" + name, contents);
    card.Folders(kNew + "/Logs");
    const MigrateReport report = AppDirLogic::Migrate(card, kRoot, kOld, kNew);
    assert(report.outcome == MigrateReport::Outcome::Merged && report.left == static_cast<int>(files.size()) && report.moved == 0);
    assert(card.Files(kOld) == files && card.Files(kNew) == files);
}

}  // namespace

int main()
{
    TestSafeFolderName();
    TestJoinAndMake();
    TestNothingToDo();
    TestRename();
    TestRenameThatReportsFailure();
    TestMerge();
    TestConflictsOfKind();
    TestBlocked();
    TestPowerCutAtEachStep(OldFolder);
    TestPowerCutAtEachStep(BothFolders);
    TestPowerCutAtEachStep([](FakeCard& card) {
        // Deeper: folders in folders, some in both places.
        card.File(kNew + "/Presets/Juno/A.ccpreset", "a");
        card.File(kOld + "/Presets/Juno/B.ccpreset", "b");
        card.File(kOld + "/Presets/Juno/Pads/C.ccpreset", "c");
        card.File(kOld + "/Presets/DX7/D.ccpreset", "d");
        card.File(kOld + "/Presets/Juno/A.ccpreset", "a old");
    });
    TestCutInsideARename();
    std::puts("app_dir: ok");
    return 0;
}
