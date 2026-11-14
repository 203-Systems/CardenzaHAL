// The firmware's app description: its project name and version (from the build), ESP-IDF's version,
// and the build's hash in place of the ELF's.

#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_idf_version.h"

#ifndef CARDENZA_WEB_PROJECT_NAME
#define CARDENZA_WEB_PROJECT_NAME "firmware"
#endif
#ifndef CARDENZA_WEB_PROJECT_VER
#define CARDENZA_WEB_PROJECT_VER "1"
#endif
#ifndef CARDENZA_WEB_BUILD_HASH
#define CARDENZA_WEB_BUILD_HASH "0000000000000000000000000000000000000000000000000000000000000000"
#endif

namespace {

esp_app_desc_t MakeDescription()
{
    esp_app_desc_t description{};
    description.magic_word = ESP_APP_DESC_MAGIC_WORD;
    std::snprintf(description.version, sizeof(description.version), "%s", CARDENZA_WEB_PROJECT_VER);
    std::snprintf(description.project_name, sizeof(description.project_name), "%s", CARDENZA_WEB_PROJECT_NAME);
    std::snprintf(description.time, sizeof(description.time), "%s", __TIME__);
    std::snprintf(description.date, sizeof(description.date), "%s", __DATE__);
    std::snprintf(description.idf_ver, sizeof(description.idf_ver), "%s", IDF_VER);
    const char* hash = CARDENZA_WEB_BUILD_HASH;
    for (int index = 0; index < 32 && hash[index * 2] != '\0' && hash[index * 2 + 1] != '\0'; ++index)
    {
        unsigned value = 0;
        std::sscanf(hash + index * 2, "%2x", &value);
        description.app_elf_sha256[index] = static_cast<uint8_t>(value);
    }
    return description;
}

}  // namespace

extern "C" {

const esp_app_desc_t* esp_app_get_description(void)
{
    static const esp_app_desc_t description = MakeDescription();
    return &description;
}

int esp_app_get_elf_sha256(char* dst, size_t size)
{
    if (dst == nullptr || size == 0) return 0;
    const esp_app_desc_t* description = esp_app_get_description();
    size_t written = 0;
    for (int index = 0; index < 32 && written + 2 < size; ++index)
    {
        std::snprintf(dst + written, 3, "%02x", description->app_elf_sha256[index]);
        written += 2;
    }
    dst[written] = '\0';
    return static_cast<int>(written + 1);
}

char* esp_app_get_elf_sha256_str(void)
{
    static char text[65];
    esp_app_get_elf_sha256(text, sizeof(text));
    return text;
}

}  // extern "C"
