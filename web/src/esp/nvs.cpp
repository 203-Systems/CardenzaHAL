// NVS on the web target: namespaces of typed keys in memory, written to /.cardenza/nvs.bin on
// nvs_commit (the page keeps that folder in IndexedDB when it may) and read back by nvs_flash_init.

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "nvs.h"
#include "nvs_flash.h"

#include "../platform.h"

namespace {

constexpr char kFolder[] = "/.cardenza";
constexpr char kFile[] = "/.cardenza/nvs.bin";
constexpr uint32_t kMagic = 0x4E565331;  // "NVS1"

struct Value {
    nvs_type_t type;
    std::vector<uint8_t> bytes;
};

struct Handle {
    std::string space;
    bool writable;
};

bool g_initialized = false;
std::map<std::string, std::map<std::string, Value>>* g_store = nullptr;
std::map<uint32_t, Handle>* g_handles = nullptr;
uint32_t g_nextHandle = 1;

std::map<std::string, std::map<std::string, Value>>& Store()
{
    if (g_store == nullptr) g_store = new std::map<std::string, std::map<std::string, Value>>();
    return *g_store;
}

std::map<uint32_t, Handle>& Handles()
{
    if (g_handles == nullptr) g_handles = new std::map<uint32_t, Handle>();
    return *g_handles;
}

void Load()
{
    Store().clear();
    FILE* file = std::fopen(kFile, "rb");
    if (file == nullptr) return;
    uint32_t magic = 0;
    if (std::fread(&magic, 4, 1, file) == 1 && magic == kMagic)
    {
        for (;;)
        {
            uint8_t lengths[2];
            uint8_t type;
            uint32_t size;
            if (std::fread(lengths, 1, 2, file) != 2 || std::fread(&type, 1, 1, file) != 1 || std::fread(&size, 4, 1, file) != 1) break;
            std::string space(lengths[0], '\0'), key(lengths[1], '\0');
            Value value{static_cast<nvs_type_t>(type), std::vector<uint8_t>(size)};
            if (std::fread(space.data(), 1, lengths[0], file) != lengths[0] || std::fread(key.data(), 1, lengths[1], file) != lengths[1] ||
                (size != 0 && std::fread(value.bytes.data(), 1, size, file) != size))
                break;
            Store()[space][key] = std::move(value);
        }
    }
    std::fclose(file);
}

esp_err_t Save()
{
    mkdir(kFolder, 0777);
    FILE* file = std::fopen(kFile, "wb");
    if (file == nullptr) return ESP_FAIL;
    std::fwrite(&kMagic, 4, 1, file);
    for (const auto& [space, keys] : Store())
        for (const auto& [key, value] : keys)
        {
            const uint8_t lengths[2] = {static_cast<uint8_t>(space.size()), static_cast<uint8_t>(key.size())};
            const uint8_t type = value.type;
            const uint32_t size = static_cast<uint32_t>(value.bytes.size());
            std::fwrite(lengths, 1, 2, file);
            std::fwrite(&type, 1, 1, file);
            std::fwrite(&size, 4, 1, file);
            std::fwrite(space.data(), 1, space.size(), file);
            std::fwrite(key.data(), 1, key.size(), file);
            if (size != 0) std::fwrite(value.bytes.data(), 1, size, file);
        }
    std::fclose(file);
    cardenza_web::platform::FilesChanged();
    return ESP_OK;
}

Handle* Get(nvs_handle_t handle)
{
    auto found = Handles().find(handle);
    return found == Handles().end() ? nullptr : &found->second;
}

esp_err_t Set(nvs_handle_t handle, const char* key, nvs_type_t type, const void* data, size_t size)
{
    Handle* h = Get(handle);
    if (h == nullptr) return ESP_ERR_NVS_INVALID_HANDLE;
    if (!h->writable) return ESP_ERR_NVS_READ_ONLY;
    if (key == nullptr || std::strlen(key) == 0 || std::strlen(key) >= NVS_KEY_NAME_MAX_SIZE) return ESP_ERR_NVS_KEY_TOO_LONG;
    const auto* bytes = static_cast<const uint8_t*>(data);
    Store()[h->space][key] = Value{type, std::vector<uint8_t>(bytes, bytes + size)};
    return ESP_OK;
}

esp_err_t GetValue(nvs_handle_t handle, const char* key, nvs_type_t type, void* out, size_t size)
{
    Handle* h = Get(handle);
    if (h == nullptr) return ESP_ERR_NVS_INVALID_HANDLE;
    auto& keys = Store()[h->space];
    auto found = keys.find(key != nullptr ? key : "");
    if (found == keys.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (found->second.type != type) return ESP_ERR_NVS_TYPE_MISMATCH;
    if (out != nullptr) std::memcpy(out, found->second.bytes.data(), size);
    return ESP_OK;
}

esp_err_t GetVariable(nvs_handle_t handle, const char* key, nvs_type_t type, void* out, size_t* length)
{
    Handle* h = Get(handle);
    if (h == nullptr) return ESP_ERR_NVS_INVALID_HANDLE;
    if (length == nullptr) return ESP_ERR_INVALID_ARG;
    auto& keys = Store()[h->space];
    auto found = keys.find(key != nullptr ? key : "");
    if (found == keys.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (found->second.type != type) return ESP_ERR_NVS_TYPE_MISMATCH;
    const size_t size = found->second.bytes.size();
    if (out == nullptr)
    {
        *length = size;
        return ESP_OK;
    }
    if (*length < size)
    {
        *length = size;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    std::memcpy(out, found->second.bytes.data(), size);
    *length = size;
    return ESP_OK;
}

}  // namespace

extern "C" {

esp_err_t nvs_flash_init(void)
{
    if (!g_initialized) Load();
    g_initialized = true;
    return ESP_OK;
}

esp_err_t nvs_flash_init_partition(const char*) { return nvs_flash_init(); }

esp_err_t nvs_flash_deinit(void)
{
    g_initialized = false;
    Handles().clear();
    return ESP_OK;
}

esp_err_t nvs_flash_deinit_partition(const char*) { return nvs_flash_deinit(); }

esp_err_t nvs_flash_erase(void)
{
    Store().clear();
    return Save();
}

esp_err_t nvs_flash_erase_partition(const char*) { return nvs_flash_erase(); }

esp_err_t nvs_open(const char* namespace_name, nvs_open_mode_t open_mode, nvs_handle_t* out_handle)
{
    if (!g_initialized) return ESP_ERR_NVS_NOT_INITIALIZED;
    if (namespace_name == nullptr || std::strlen(namespace_name) == 0 || std::strlen(namespace_name) >= NVS_NS_NAME_MAX_SIZE)
        return ESP_ERR_NVS_INVALID_NAME;
    if (open_mode == NVS_READONLY && Store().find(namespace_name) == Store().end()) return ESP_ERR_NVS_NOT_FOUND;
    const nvs_handle_t handle = g_nextHandle++;
    Handles()[handle] = Handle{namespace_name, open_mode == NVS_READWRITE};
    *out_handle = handle;
    return ESP_OK;
}

esp_err_t nvs_open_from_partition(const char*, const char* namespace_name, nvs_open_mode_t open_mode, nvs_handle_t* out_handle)
{
    return nvs_open(namespace_name, open_mode, out_handle);
}

esp_err_t nvs_set_i8(nvs_handle_t h, const char* key, int8_t value) { return Set(h, key, NVS_TYPE_I8, &value, sizeof(value)); }
esp_err_t nvs_set_u8(nvs_handle_t h, const char* key, uint8_t value) { return Set(h, key, NVS_TYPE_U8, &value, sizeof(value)); }
esp_err_t nvs_set_i16(nvs_handle_t h, const char* key, int16_t value) { return Set(h, key, NVS_TYPE_I16, &value, sizeof(value)); }
esp_err_t nvs_set_u16(nvs_handle_t h, const char* key, uint16_t value) { return Set(h, key, NVS_TYPE_U16, &value, sizeof(value)); }
esp_err_t nvs_set_i32(nvs_handle_t h, const char* key, int32_t value) { return Set(h, key, NVS_TYPE_I32, &value, sizeof(value)); }
esp_err_t nvs_set_u32(nvs_handle_t h, const char* key, uint32_t value) { return Set(h, key, NVS_TYPE_U32, &value, sizeof(value)); }
esp_err_t nvs_set_i64(nvs_handle_t h, const char* key, int64_t value) { return Set(h, key, NVS_TYPE_I64, &value, sizeof(value)); }
esp_err_t nvs_set_u64(nvs_handle_t h, const char* key, uint64_t value) { return Set(h, key, NVS_TYPE_U64, &value, sizeof(value)); }

esp_err_t nvs_set_str(nvs_handle_t h, const char* key, const char* value)
{
    if (value == nullptr) return ESP_ERR_INVALID_ARG;
    return Set(h, key, NVS_TYPE_STR, value, std::strlen(value) + 1);
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char* key, const void* value, size_t length) { return Set(h, key, NVS_TYPE_BLOB, value, length); }
esp_err_t nvs_get_i8(nvs_handle_t h, const char* key, int8_t* out) { return GetValue(h, key, NVS_TYPE_I8, out, sizeof(*out)); }
esp_err_t nvs_get_u8(nvs_handle_t h, const char* key, uint8_t* out) { return GetValue(h, key, NVS_TYPE_U8, out, sizeof(*out)); }
esp_err_t nvs_get_i16(nvs_handle_t h, const char* key, int16_t* out) { return GetValue(h, key, NVS_TYPE_I16, out, sizeof(*out)); }
esp_err_t nvs_get_u16(nvs_handle_t h, const char* key, uint16_t* out) { return GetValue(h, key, NVS_TYPE_U16, out, sizeof(*out)); }
esp_err_t nvs_get_i32(nvs_handle_t h, const char* key, int32_t* out) { return GetValue(h, key, NVS_TYPE_I32, out, sizeof(*out)); }
esp_err_t nvs_get_u32(nvs_handle_t h, const char* key, uint32_t* out) { return GetValue(h, key, NVS_TYPE_U32, out, sizeof(*out)); }
esp_err_t nvs_get_i64(nvs_handle_t h, const char* key, int64_t* out) { return GetValue(h, key, NVS_TYPE_I64, out, sizeof(*out)); }
esp_err_t nvs_get_u64(nvs_handle_t h, const char* key, uint64_t* out) { return GetValue(h, key, NVS_TYPE_U64, out, sizeof(*out)); }
esp_err_t nvs_get_str(nvs_handle_t h, const char* key, char* out, size_t* length) { return GetVariable(h, key, NVS_TYPE_STR, out, length); }
esp_err_t nvs_get_blob(nvs_handle_t h, const char* key, void* out, size_t* length) { return GetVariable(h, key, NVS_TYPE_BLOB, out, length); }

esp_err_t nvs_find_key(nvs_handle_t handle, const char* key, nvs_type_t* out_type)
{
    Handle* h = Get(handle);
    if (h == nullptr) return ESP_ERR_NVS_INVALID_HANDLE;
    auto& keys = Store()[h->space];
    auto found = keys.find(key != nullptr ? key : "");
    if (found == keys.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (out_type != nullptr) *out_type = found->second.type;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key)
{
    Handle* h = Get(handle);
    if (h == nullptr) return ESP_ERR_NVS_INVALID_HANDLE;
    if (!h->writable) return ESP_ERR_NVS_READ_ONLY;
    return Store()[h->space].erase(key != nullptr ? key : "") != 0 ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    Handle* h = Get(handle);
    if (h == nullptr) return ESP_ERR_NVS_INVALID_HANDLE;
    if (!h->writable) return ESP_ERR_NVS_READ_ONLY;
    Store()[h->space].clear();
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    if (Get(handle) == nullptr) return ESP_ERR_NVS_INVALID_HANDLE;
    return Save();
}

void nvs_close(nvs_handle_t handle) { Handles().erase(handle); }

}  // extern "C"
