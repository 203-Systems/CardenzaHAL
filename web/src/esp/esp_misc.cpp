// The small parts of ESP-IDF a firmware uses everywhere: the log, error names, restart and reset
// reason, chip identity, random numbers, the MAC, ROM helpers, the task watchdog and FreeRTOS hooks.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_chip_info.h"
#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_freertos_hooks.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "../kernel/kernel.h"
#include "../platform.h"

namespace k = cardenza_web::kernel;

namespace {

struct TagLevel {
    char tag[24];
    esp_log_level_t level;
};
constexpr int kTagLevels = 32;
TagLevel g_tagLevels[kTagLevels];
int g_tagLevelCount = 0;
esp_log_level_t g_defaultLevel = static_cast<esp_log_level_t>(CONFIG_LOG_DEFAULT_LEVEL);
esp_log_level_t g_masterLevel = ESP_LOG_VERBOSE;
vprintf_like_t g_vprintf = vprintf;

constexpr int kShutdownHandlers = 8;
shutdown_handler_t g_shutdownHandlers[kShutdownHandlers];

constexpr int kHooks = 8;
esp_freertos_idle_cb_t g_idleHooks[kHooks];

void RunIdleHooks()
{
    for (esp_freertos_idle_cb_t hook : g_idleHooks)
        if (hook != nullptr) hook();
}

esp_log_level_t LevelFor(const char* tag)
{
    if (tag != nullptr)
        for (int index = 0; index < g_tagLevelCount; ++index)
            if (std::strcmp(g_tagLevels[index].tag, tag) == 0) return g_tagLevels[index].level;
    return g_defaultLevel;
}

struct ErrorName {
    esp_err_t code;
    const char* name;
};

constexpr ErrorName kErrorNames[] = {
    {ESP_OK, "ESP_OK"},
    {ESP_FAIL, "ESP_FAIL"},
    {ESP_ERR_NO_MEM, "ESP_ERR_NO_MEM"},
    {ESP_ERR_INVALID_ARG, "ESP_ERR_INVALID_ARG"},
    {ESP_ERR_INVALID_STATE, "ESP_ERR_INVALID_STATE"},
    {ESP_ERR_INVALID_SIZE, "ESP_ERR_INVALID_SIZE"},
    {ESP_ERR_NOT_FOUND, "ESP_ERR_NOT_FOUND"},
    {ESP_ERR_NOT_SUPPORTED, "ESP_ERR_NOT_SUPPORTED"},
    {ESP_ERR_TIMEOUT, "ESP_ERR_TIMEOUT"},
    {ESP_ERR_INVALID_RESPONSE, "ESP_ERR_INVALID_RESPONSE"},
    {ESP_ERR_INVALID_CRC, "ESP_ERR_INVALID_CRC"},
    {ESP_ERR_INVALID_VERSION, "ESP_ERR_INVALID_VERSION"},
    {ESP_ERR_INVALID_MAC, "ESP_ERR_INVALID_MAC"},
    {ESP_ERR_NOT_FINISHED, "ESP_ERR_NOT_FINISHED"},
    {ESP_ERR_NOT_ALLOWED, "ESP_ERR_NOT_ALLOWED"},
    {0x1100, "ESP_ERR_NVS_NOT_INITIALIZED"},
    {0x1102, "ESP_ERR_NVS_NOT_FOUND"},
    {0x1103, "ESP_ERR_NVS_TYPE_MISMATCH"},
    {0x1104, "ESP_ERR_NVS_READ_ONLY"},
    {0x1105, "ESP_ERR_NVS_NOT_ENOUGH_SPACE"},
    {0x1106, "ESP_ERR_NVS_INVALID_NAME"},
    {0x1107, "ESP_ERR_NVS_INVALID_HANDLE"},
    {0x110c, "ESP_ERR_NVS_INVALID_LENGTH"},
    {0x110d, "ESP_ERR_NVS_NO_FREE_PAGES"},
    {0x1110, "ESP_ERR_NVS_NEW_VERSION_FOUND"},
};

}  // namespace

namespace cardenza_web::esp {
void InstallIdleHooks() { k::SetIdleHook(RunIdleHooks); }
}  // namespace cardenza_web::esp

extern "C" {

// ---- log --------------------------------------------------------------------------------------------

void esp_log_set_level_master(esp_log_level_t level) { g_masterLevel = level; }
esp_log_level_t esp_log_get_level_master(void) { return g_masterLevel; }

void esp_log_level_set(const char* tag, esp_log_level_t level)
{
    if (tag == nullptr) return;
    if (std::strcmp(tag, "*") == 0)
    {
        g_defaultLevel = level;
        g_tagLevelCount = 0;
        return;
    }
    for (int index = 0; index < g_tagLevelCount; ++index)
        if (std::strcmp(g_tagLevels[index].tag, tag) == 0)
        {
            g_tagLevels[index].level = level;
            return;
        }
    if (g_tagLevelCount < kTagLevels)
    {
        std::snprintf(g_tagLevels[g_tagLevelCount].tag, sizeof(g_tagLevels[0].tag), "%s", tag);
        g_tagLevels[g_tagLevelCount++].level = level;
    }
}

esp_log_level_t esp_log_level_get(const char* tag) { return LevelFor(tag); }

vprintf_like_t esp_log_set_vprintf(vprintf_like_t func)
{
    const vprintf_like_t previous = g_vprintf;
    g_vprintf = func != nullptr ? func : vprintf;
    return previous;
}

uint32_t esp_log_timestamp(void) { return static_cast<uint32_t>(k::NowUs() / 1000); }
uint32_t esp_log_early_timestamp(void) { return esp_log_timestamp(); }

char* esp_log_system_timestamp(void)
{
    static char buffer[24];
    const uint64_t ms = k::NowUs() / 1000;
    std::snprintf(buffer, sizeof(buffer), "%02u:%02u:%02u.%03u", static_cast<unsigned>(ms / 3600000 % 24),
                  static_cast<unsigned>(ms / 60000 % 60), static_cast<unsigned>(ms / 1000 % 60), static_cast<unsigned>(ms % 1000));
    return buffer;
}

void esp_log_writev(esp_log_level_t level, const char* tag, const char* format, va_list args)
{
    if (level > g_masterLevel || level > LevelFor(tag)) return;
    g_vprintf(format, args);
}

void esp_log_write(esp_log_level_t level, const char* tag, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    esp_log_writev(level, tag, format, args);
    va_end(args);
}

void esp_log_buffer_hex_internal(const char* tag, const void* buffer, uint16_t length, esp_log_level_t level)
{
    const auto* bytes = static_cast<const uint8_t*>(buffer);
    for (uint16_t at = 0; at < length; at += 16)
    {
        char line[16 * 3 + 1] = {};
        for (uint16_t index = 0; index < 16 && at + index < length; ++index)
            std::snprintf(line + index * 3, 4, "%02x ", bytes[at + index]);
        esp_log_write(level, tag, "%c (%u) %s: %s\n", "NEWIDV"[level], static_cast<unsigned>(esp_log_timestamp()), tag, line);
    }
}

void esp_log_buffer_char_internal(const char* tag, const void* buffer, uint16_t length, esp_log_level_t level)
{
    const auto* bytes = static_cast<const char*>(buffer);
    for (uint16_t at = 0; at < length; at += 16)
    {
        char line[17] = {};
        for (uint16_t index = 0; index < 16 && at + index < length; ++index) line[index] = bytes[at + index];
        esp_log_write(level, tag, "%c (%u) %s: %s\n", "NEWIDV"[level], static_cast<unsigned>(esp_log_timestamp()), tag, line);
    }
}

void esp_log_buffer_hexdump_internal(const char* tag, const void* buffer, uint16_t length, esp_log_level_t level)
{
    const auto* bytes = static_cast<const uint8_t*>(buffer);
    for (uint16_t at = 0; at < length; at += 16)
    {
        char hex[16 * 3 + 2] = {};
        char text[17] = {};
        for (uint16_t index = 0; index < 16; ++index)
        {
            if (at + index < length)
            {
                std::snprintf(hex + index * 3, 4, "%02x ", bytes[at + index]);
                const char c = static_cast<char>(bytes[at + index]);
                text[index] = c >= 32 && c < 127 ? c : '.';
            }
            else
            {
                std::memcpy(hex + index * 3, "   ", 3);
            }
        }
        esp_log_write(level, tag, "%c (%u) %s: %p  %s |%s|\n", "NEWIDV"[level], static_cast<unsigned>(esp_log_timestamp()), tag,
                      static_cast<const void*>(bytes + at), hex, text);
    }
}

// ---- errors -----------------------------------------------------------------------------------------

const char* esp_err_to_name(esp_err_t code)
{
    for (const ErrorName& entry : kErrorNames)
        if (entry.code == code) return entry.name;
    return "UNKNOWN ERROR";
}

const char* esp_err_to_name_r(esp_err_t code, char* buf, size_t buflen)
{
    const char* name = esp_err_to_name(code);
    if (std::strcmp(name, "UNKNOWN ERROR") == 0) std::snprintf(buf, buflen, "Unknown error 0x%x", static_cast<unsigned>(code));
    else std::snprintf(buf, buflen, "%s", name);
    return buf;
}

void _esp_error_check_failed(esp_err_t rc, const char* file, int line, const char* function, const char* expression)
{
    std::fprintf(stderr, "ESP_ERROR_CHECK failed: esp_err_t 0x%x (%s) at %p\nfile: \"%s\" line %d\nfunc: %s\nexpression: %s\n",
                 static_cast<unsigned>(rc), esp_err_to_name(rc), __builtin_return_address(0), file, line, function, expression);
    std::fflush(stderr);
    std::abort();
}

void _esp_error_check_failed_without_abort(esp_err_t rc, const char* file, int line, const char* function, const char* expression)
{
    std::fprintf(stderr, "ESP_ERROR_CHECK_WITHOUT_ABORT failed: esp_err_t 0x%x (%s)\nfile: \"%s\" line %d\nfunc: %s\nexpression: %s\n",
                 static_cast<unsigned>(rc), esp_err_to_name(rc), file, line, function, expression);
}

// ---- system -----------------------------------------------------------------------------------------

esp_err_t esp_register_shutdown_handler(shutdown_handler_t handle)
{
    for (shutdown_handler_t& slot : g_shutdownHandlers)
    {
        if (slot == handle) return ESP_ERR_INVALID_STATE;
        if (slot == nullptr)
        {
            slot = handle;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t esp_unregister_shutdown_handler(shutdown_handler_t handle)
{
    for (shutdown_handler_t& slot : g_shutdownHandlers)
        if (slot == handle)
        {
            slot = nullptr;
            return ESP_OK;
        }
    return ESP_ERR_INVALID_STATE;
}

void esp_restart(void)
{
    for (int index = kShutdownHandlers - 1; index >= 0; --index)
        if (g_shutdownHandlers[index] != nullptr) g_shutdownHandlers[index]();
    cardenza_web::platform::Restart(ESP_RST_SW);
}

esp_reset_reason_t esp_reset_reason(void) { return cardenza_web::platform::ResetReason(); }

void esp_system_abort(const char* details)
{
    std::fprintf(stderr, "abort() was called: %s\n", details != nullptr ? details : "");
    std::fflush(stderr);
    std::abort();
}

const char* esp_get_idf_version(void) { return IDF_VER; }

void esp_chip_info(esp_chip_info_t* out_info)
{
    out_info->model = CHIP_ESP32S3;
    out_info->features = CHIP_FEATURE_WIFI_BGN | CHIP_FEATURE_BLE | CHIP_FEATURE_EMB_PSRAM;
    out_info->revision = 2;
    out_info->cores = 2;
}

uint32_t esp_random(void) { return cardenza_web::platform::Random32(); }

void esp_fill_random(void* buf, size_t len)
{
    auto* bytes = static_cast<uint8_t*>(buf);
    for (size_t index = 0; index < len; index += 4)
    {
        const uint32_t value = esp_random();
        for (size_t byte = 0; byte < 4 && index + byte < len; ++byte) bytes[index + byte] = static_cast<uint8_t>(value >> (byte * 8));
    }
}

// A locally administered address, the same for every simulator.
static const uint8_t kMac[6] = {0x02, 0x03, 0xC0, 0xDE, 0x20, 0x30};

esp_err_t esp_read_mac(uint8_t* mac, esp_mac_type_t type)
{
    if (mac == nullptr) return ESP_ERR_INVALID_ARG;
    std::memcpy(mac, kMac, 6);
    if (type == ESP_MAC_WIFI_SOFTAP) mac[5] += 1;
    else if (type == ESP_MAC_BT) mac[5] += 2;
    else if (type == ESP_MAC_ETH) mac[5] += 3;
    return ESP_OK;
}

esp_err_t esp_efuse_mac_get_default(uint8_t* mac) { return esp_read_mac(mac, ESP_MAC_BASE); }
esp_err_t esp_base_mac_addr_get(uint8_t* mac) { return esp_read_mac(mac, ESP_MAC_BASE); }

esp_err_t esp_derive_local_mac(uint8_t* local_mac, const uint8_t* universal_mac)
{
    if (local_mac == nullptr || universal_mac == nullptr) return ESP_ERR_INVALID_ARG;
    std::memcpy(local_mac, universal_mac, 6);
    local_mac[0] |= 0x02;
    return ESP_OK;
}

// ---- ROM and CPU ------------------------------------------------------------------------------------

int esp_rom_printf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    const int written = std::vprintf(fmt, args);
    va_end(args);
    return written;
}

void esp_rom_delay_us(uint32_t us)
{
    // A busy wait on the device: here time passes without giving the CPU away, as there.
    const uint64_t until = k::NowUs() + us;
    while (k::NowUs() < until) {}
}

void esp_rom_install_channel_putc(int, void (*)(char)) {}
void esp_rom_install_uart_printf(void) {}
uint32_t esp_rom_get_cpu_ticks_per_us(void) { return 240; }
void esp_rom_software_reset_system(void) { cardenza_web::platform::Restart(ESP_RST_SW); }

int esp_cpu_get_core_id(void) { return k::CurrentCore(); }
esp_cpu_cycle_count_t esp_cpu_get_cycle_count(void) { return static_cast<esp_cpu_cycle_count_t>(k::NowUs() * 240); }
void esp_cpu_set_cycle_count(esp_cpu_cycle_count_t) {}

void* esp_cpu_get_sp(void) { return __builtin_frame_address(0); }

void esp_cpu_wait_for_intr(void) { k::Yield(); }
bool esp_cpu_dbgr_is_attached(void) { return false; }
void esp_cpu_dbgr_break(void) {}

// ---- the task watchdog: accepted, never fires ---------------------------------------------------

esp_err_t esp_task_wdt_init(const esp_task_wdt_config_t*) { return ESP_OK; }
esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t*) { return ESP_OK; }
esp_err_t esp_task_wdt_deinit(void) { return ESP_OK; }
esp_err_t esp_task_wdt_add(TaskHandle_t) { return ESP_OK; }

esp_err_t esp_task_wdt_add_user(const char*, esp_task_wdt_user_handle_t* user_handle_ret)
{
    static int user;
    if (user_handle_ret != nullptr) *user_handle_ret = reinterpret_cast<esp_task_wdt_user_handle_t>(&user);
    return ESP_OK;
}

esp_err_t esp_task_wdt_reset(void)
{
    k::PreemptionPoint();
    return ESP_OK;
}

esp_err_t esp_task_wdt_reset_user(esp_task_wdt_user_handle_t) { return esp_task_wdt_reset(); }
esp_err_t esp_task_wdt_delete(TaskHandle_t) { return ESP_OK; }
esp_err_t esp_task_wdt_delete_user(esp_task_wdt_user_handle_t) { return ESP_OK; }
esp_err_t esp_task_wdt_status(TaskHandle_t) { return ESP_ERR_NOT_FOUND; }
esp_err_t esp_task_wdt_print_triggered_tasks(void*, void*, int*) { return ESP_FAIL; }

// ---- FreeRTOS hooks -----------------------------------------------------------------------------

esp_err_t esp_register_freertos_idle_hook_for_cpu(esp_freertos_idle_cb_t new_idle_cb, UBaseType_t)
{
    for (esp_freertos_idle_cb_t& slot : g_idleHooks)
        if (slot == nullptr)
        {
            slot = new_idle_cb;
            return ESP_OK;
        }
    return ESP_ERR_NO_MEM;
}

esp_err_t esp_register_freertos_idle_hook(esp_freertos_idle_cb_t new_idle_cb) { return esp_register_freertos_idle_hook_for_cpu(new_idle_cb, 0); }
// No tick interrupt runs on the web target: tick hooks are accepted and never called.
esp_err_t esp_register_freertos_tick_hook_for_cpu(esp_freertos_tick_cb_t, UBaseType_t) { return ESP_OK; }
esp_err_t esp_register_freertos_tick_hook(esp_freertos_tick_cb_t) { return ESP_OK; }

void esp_deregister_freertos_idle_hook_for_cpu(esp_freertos_idle_cb_t old_idle_cb, UBaseType_t)
{
    for (esp_freertos_idle_cb_t& slot : g_idleHooks)
        if (slot == old_idle_cb) slot = nullptr;
}

void esp_deregister_freertos_idle_hook(esp_freertos_idle_cb_t old_idle_cb) { esp_deregister_freertos_idle_hook_for_cpu(old_idle_cb, 0); }
void esp_deregister_freertos_tick_hook_for_cpu(esp_freertos_tick_cb_t, UBaseType_t) {}
void esp_deregister_freertos_tick_hook(esp_freertos_tick_cb_t) {}

}  // extern "C"
