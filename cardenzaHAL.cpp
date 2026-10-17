#include "cardenzaHAL.h"

#include <esp_log.h>

namespace {

CardenzaHAL::InitReport g_initReport;
bool g_initialized = false;
bool g_keyboardInitialized = false;

constexpr char kLogTag[] = "CardenzaHAL";

void LogModuleResult(const char* moduleName, bool initialized, bool optional)
{
    if (initialized)
    {
        ESP_LOGI(kLogTag, "%s initialized", moduleName);
        return;
    }

    if (optional)
    {
        ESP_LOGW(kLogTag, "%s unavailable", moduleName);
    }
    else
    {
        ESP_LOGE(kLogTag, "%s failed", moduleName);
    }
}

}  // namespace

namespace CardenzaHAL {

bool Init()
{
    if (g_initialized)
    {
        return g_initReport.overall;
    }

    g_initReport = {};

    g_initReport.system = System::Init();
    LogModuleResult("system", g_initReport.system, false);
    if (!g_initReport.system)
    {
        g_initReport.overall = false;
        return g_initReport.overall;
    }

    if (!g_keyboardInitialized)
    {
        Keyboard::init();
        g_keyboardInitialized = true;
        ESP_LOGI(kLogTag, "keyboard initialized");
    }

    g_initReport.audio = Audio::Init();
    LogModuleResult("audio", g_initReport.audio, false);

    g_initReport.display = Display::Init();
    LogModuleResult("display", g_initReport.display, false);

    g_initReport.storage = Storage::Init();
    LogModuleResult("storage", g_initReport.storage, true);

    g_initReport.ir = IR::Init();
    LogModuleResult("ir", g_initReport.ir, true);

    g_initReport.overall = g_initReport.system && g_initReport.audio && g_initReport.display;
    g_initialized = g_initReport.overall;
    return g_initReport.overall;
}

const InitReport& LastInitReport()
{
    return g_initReport;
}

}  // namespace CardenzaHAL
