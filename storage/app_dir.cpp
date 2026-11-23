#include "app_dir.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

#include <esp_app_desc.h>
#include <esp_log.h>
#include <sdkconfig.h>

namespace CardenzaHAL::Storage {

namespace {

constexpr char kTag[] = "CardenzaStorage";

#ifndef CONFIG_CARDENZA_HAL_APP_NAME
#define CONFIG_CARDENZA_HAL_APP_NAME ""
#endif

// The card through ESP-IDF's VFS (FAT): every call is one FatFs operation.
class CardFileSystem final : public AppDirLogic::FileSystem {
public:
    Kind Stat(const std::string& path) override
    {
        struct stat info{};
        if (stat(path.c_str(), &info) != 0) return Kind::None;
        return S_ISDIR(info.st_mode) ? Kind::Directory : Kind::File;
    }

    bool List(const std::string& path, std::vector<std::string>& names) override
    {
        DIR* directory = opendir(path.c_str());
        if (directory == nullptr) return false;
        while (const dirent* entry = readdir(directory))
        {
            const std::string name = entry->d_name;
            if (!name.empty() && name != "." && name != "..") names.push_back(name);
        }
        closedir(directory);
        return true;
    }

    bool Rename(const std::string& from, const std::string& to) override
    {
        // FAT never renames over an existing name (EEXIST): nothing is overwritten.
        return std::rename(from.c_str(), to.c_str()) == 0;
    }

    bool MakeDirectory(const std::string& path) override { return mkdir(path.c_str(), 0775) == 0; }

    bool RemoveDirectory(const std::string& path) override { return rmdir(path.c_str()) == 0; }
};

// "/sdcard/x", "/x" and "x" all name /sdcard/x.
std::string CardPath(std::string_view path)
{
    const std::string_view mount = kMountPoint;
    if (path.size() > mount.size() && path.compare(0, mount.size(), mount) == 0 && path[mount.size()] == '/')
        path.remove_prefix(mount.size());
    while (!path.empty() && path.front() == '/') path.remove_prefix(1);
    while (!path.empty() && path.back() == '/') path.remove_suffix(1);
    return path.empty() ? std::string() : AppDirLogic::JoinPath(kMountPoint, path);
}

}  // namespace

std::string AppName()
{
    std::string name = AppDirLogic::SafeFolderName(CONFIG_CARDENZA_HAL_APP_NAME);
    if (name.empty())
    {
        const esp_app_desc_t* app = esp_app_get_description();
        if (app != nullptr) name = AppDirLogic::SafeFolderName(std::string_view(app->project_name, strnlen(app->project_name, sizeof(app->project_name))));
    }
    return name.empty() ? std::string("App") : name;
}

std::string AppDir()
{
    return AppDirLogic::JoinPath(kAppsDir, AppName());
}

std::string AppPath(std::string_view relative)
{
    return AppDirLogic::JoinPath(AppDir(), relative);
}

bool EnsureAppDir(std::string_view subfolder)
{
    if (!IsMounted()) return false;
    CardFileSystem card;
    // From the card's root: the mount point itself is not a folder FAT can stat or make.
    const std::string relative = AppDirLogic::JoinPath(AppDir().substr(std::strlen(kMountPoint) + 1), subfolder);
    return AppDirLogic::MakeDirectories(card, kMountPoint, relative);
}

MigrateReport MigrateAppDir(std::string_view oldDir)
{
    MigrateReport report;
    const std::string from = CardPath(oldDir);
    if (!IsMounted())
    {
        report.outcome = MigrateReport::Outcome::NoCard;
        return report;
    }
    if (from.empty()) return report;
    CardFileSystem card;
    const std::string to = AppDir();
    report = AppDirLogic::Migrate(card, kMountPoint, from, to);
    const std::string line = AppDirLogic::Describe(report, from, to);
    if (report.outcome == MigrateReport::Outcome::Failed) ESP_LOGW(kTag, "%s", line.c_str());
    else if (!line.empty()) ESP_LOGI(kTag, "%s", line.c_str());
    return report;
}

}  // namespace CardenzaHAL::Storage
