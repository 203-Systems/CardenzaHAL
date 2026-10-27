#include "es8156.h"

#include "../../system/system.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace CardenzaHAL::Audio::Codec::ES8156 {

namespace {

bool g_initialized = false;
bool g_enabled = false;
uint8_t g_volume = 100;

struct RegisterValue {
    uint8_t reg;
    uint8_t value;
};

constexpr uint16_t kCodecAddress = 0x08;
constexpr uint32_t kI2cSpeedHz = 10000;
constexpr int kI2cTimeoutMs = 250;
constexpr int kStartupSettleDelayUs = 10000;
constexpr uint8_t kResetControlRunSequenceDisabled = 0x02;
constexpr uint8_t kResetControlStartStateMachine = 0x03;
constexpr uint8_t kModeConfigUseSclkSoftwareSlave = 0x84;
constexpr uint8_t kClockControlRatio64 = 0xA1;
constexpr uint8_t kMiscControlRatio64 = 0x02;
constexpr uint8_t kMasterLrckDividerHigh = 0x00;
constexpr uint8_t kMasterLrckDividerLow = 0x40;
constexpr uint8_t kMasterSclkDivider = 0x04;
constexpr uint8_t kMuteControlNormal = 0x00;
constexpr uint8_t kInterfaceConfigI2s16Bit = 0x30;
constexpr uint8_t kLineOutAnalogSystem2 = 0x02;
constexpr uint8_t kLineOutAnalogSystem3 = 0x40;
constexpr uint8_t kVolumeRegister = 0x14;
constexpr uint8_t kUnityGainRegister = 0xBF;
constexpr uint8_t kChipIdRegisterMsb = 0xFD;
constexpr uint8_t kChipIdRegisterMid = 0xFE;
constexpr uint8_t kChipIdRegisterLsb = 0xFF;
constexpr uint8_t kStateConfirmRegister = 0x0C;
constexpr uint8_t kExpectedChipIdMsb = 0x81;
constexpr uint8_t kExpectedChipIdMid = 0x55;
constexpr uint8_t kExpectedStateConfirm = 0x60;

constexpr RegisterValue kInitSequence[] = {
    {0x14, 0x00}, // Mute before restoring analog output paths.
    {0x02, kModeConfigUseSclkSoftwareSlave},
    {0x13, kMuteControlNormal},
    {0x20, 0x2A},
    {0x21, 0x3C},
    {0x22, kLineOutAnalogSystem2},
    {0x24, 0x07},
    {0x23, kLineOutAnalogSystem3},
    {0x0A, 0x01},
    {0x0B, 0x01},
    {0x11, kInterfaceConfigI2s16Bit},
    {0x01, kClockControlRatio64},
    {0x09, kMiscControlRatio64},
    {0x03, kMasterLrckDividerHigh},
    {0x04, kMasterLrckDividerLow},
    {0x05, kMasterSclkDivider},
    {0x0D, 0x14},
    {0x18, 0x00},
    {0x08, 0x3F},
    {0x00, kResetControlRunSequenceDisabled},
    {0x00, kResetControlStartStateMachine},
    {0x25, 0x20},
};

constexpr RegisterValue kStandbySequence[] = {
    {0x14, 0x00},
    {0x19, 0x02},
    {0x25, 0xA1},
    {0x18, 0x01},
    {0x09, 0x02},
    {0x09, 0x01},
    {0x08, 0x00},
};

i2c_master_dev_handle_t g_codecHandle = nullptr;

constexpr char kLogTag[] = "ES8156";

#ifdef ESP_ERR_INVALID_STATE
constexpr esp_err_t kInvalidState = ESP_ERR_INVALID_STATE;
#else
constexpr esp_err_t kInvalidState = ESP_FAIL;
#endif

esp_err_t EnsureDeviceHandle();
esp_err_t ReadRegister(uint8_t reg, uint8_t* value);

bool ReadChipIdRegisters(uint8_t* chipIdMsb, uint8_t* chipIdMid, uint8_t* chipIdLsb)
{
    if (chipIdMsb == nullptr || chipIdMid == nullptr || chipIdLsb == nullptr)
    {
        return false;
    }

    if (ReadRegister(kChipIdRegisterMsb, chipIdMsb) != ESP_OK ||
        ReadRegister(kChipIdRegisterMid, chipIdMid) != ESP_OK ||
        ReadRegister(kChipIdRegisterLsb, chipIdLsb) != ESP_OK)
    {
        ESP_LOGW(kLogTag, "Failed to read ES8156 identity registers after probe");
        return false;
    }

    return true;
}

bool LogAndValidateChipIdRegisters()
{
    uint8_t chipIdMsb = 0;
    uint8_t chipIdMid = 0;
    uint8_t chipIdLsb = 0;
    if (!ReadChipIdRegisters(&chipIdMsb, &chipIdMid, &chipIdLsb))
    {
        return false;
    }

    ESP_LOGI(kLogTag, "ES8156 ID: 0x%02X 0x%02X 0x%02X",
             chipIdMsb,
             chipIdMid,
             chipIdLsb);

    const bool chipIdMatches = chipIdMsb == kExpectedChipIdMsb && chipIdMid == kExpectedChipIdMid;
    if (!chipIdMatches)
    {
        ESP_LOGW(kLogTag, "ES8156 ID prefix mismatch, expected 0x%02X 0x%02X xx from the datasheet",
                 kExpectedChipIdMsb,
                 kExpectedChipIdMid);
    }
    return chipIdMatches;
}

bool LogStartupStateConfirm()
{
    uint8_t stateConfirm = 0;
    const esp_err_t stateStatus = ReadRegister(kStateConfirmRegister, &stateConfirm);
    if (stateStatus != ESP_OK)
    {
        ESP_LOGW(kLogTag, "Failed to read ES8156 state register after startup");
        return false;
    }

    if (stateConfirm != kExpectedStateConfirm)
    {
        ESP_LOGW(kLogTag, "ES8156 startup state is 0x%02X, datasheet normal-state value is 0x%02X",
                 stateConfirm,
                 kExpectedStateConfirm);
        return false;
    }
    ESP_LOGI(kLogTag, "ES8156 setup success");
    return true;
}

esp_err_t EnsureDeviceHandle()
{
    if (g_codecHandle != nullptr)
    {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = CardenzaHAL::System::GetI2CBus();
    if (bus == nullptr)
    {
        return kInvalidState;
    }

    i2c_device_config_t deviceConfig = {};
    deviceConfig.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    deviceConfig.device_address = kCodecAddress;
    deviceConfig.scl_speed_hz = kI2cSpeedHz;
    deviceConfig.scl_wait_us = 0;

    const esp_err_t status = i2c_master_bus_add_device(bus, &deviceConfig, &g_codecHandle);
    if (status != ESP_OK)
    {
        ESP_LOGE(kLogTag, "Failed to bind ES8156 on I2C bus: %s", esp_err_to_name(status));
        g_codecHandle = nullptr;
    }
    return status;
}

esp_err_t WriteRegister(uint8_t reg, uint8_t value)
{
    const esp_err_t handleStatus = EnsureDeviceHandle();
    if (handleStatus != ESP_OK)
    {
        return handleStatus;
    }

    const uint8_t payload[] = {reg, value};
    const esp_err_t status = i2c_master_transmit(g_codecHandle, payload, sizeof(payload), kI2cTimeoutMs);
    if (status != ESP_OK)
    {
        ESP_LOGE(kLogTag, "Failed to write ES8156 reg 0x%02X: %s", reg, esp_err_to_name(status));
    }
    return status;
}

esp_err_t ReadRegister(uint8_t reg, uint8_t* value)
{
    if (value == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t handleStatus = EnsureDeviceHandle();
    if (handleStatus != ESP_OK)
    {
        return handleStatus;
    }

    return i2c_master_transmit_receive(g_codecHandle, &reg, sizeof(reg), value, 1, kI2cTimeoutMs);
}

esp_err_t WriteSequence(const RegisterValue* sequence, size_t count)
{
    for (size_t index = 0; index < count; ++index)
    {
        const esp_err_t status = WriteRegister(sequence[index].reg, sequence[index].value);
        if (status != ESP_OK)
        {
            return status;
        }
    }
    return ESP_OK;
}

esp_err_t WriteStartupSequence()
{
    return WriteSequence(kInitSequence, sizeof(kInitSequence) / sizeof(kInitSequence[0]));
}

uint8_t MapVolumeRegister(uint8_t volume)
{
    return static_cast<uint8_t>((static_cast<unsigned>(volume) * kUnityGainRegister + 50U) / 100U);
}

}  // namespace

bool Init()
{
    g_initialized = false;
    g_enabled = false;

    if (!LogAndValidateChipIdRegisters())
    {
        return false;
    }

    const esp_err_t status = WriteStartupSequence();
    if (status != ESP_OK)
    {
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(kStartupSettleDelayUs / 1000));
    if (!LogStartupStateConfirm() || WriteRegister(kVolumeRegister, MapVolumeRegister(g_volume)) != ESP_OK)
        return false;

    g_initialized = true;
    g_enabled = true;
    return true;
}

bool SetEnabled(bool enabled)
{
    if (enabled && !g_initialized)
    {
        return Init();
    }

    if (!g_initialized)
    {
        g_enabled = false;
        return true;
    }

    if (g_enabled == enabled)
    {
        return true;
    }

    if (enabled) return Init();
    const esp_err_t status = WriteSequence(kStandbySequence, sizeof(kStandbySequence) / sizeof(kStandbySequence[0]));
    if (status != ESP_OK)
    {
        return false;
    }

    g_enabled = enabled;
    if (g_enabled)
    {
        return SetVolume(g_volume);
    }
    return true;
}

bool IsEnabled()
{
    return g_enabled;
}

bool SetVolume(uint8_t volume)
{
    if (volume > 100)
    {
        volume = 100;
    }

    g_volume = volume;

    if (!g_initialized || !g_enabled)
    {
        return true;
    }

    return WriteRegister(kVolumeRegister, MapVolumeRegister(volume)) == ESP_OK;
}

uint8_t GetVolume()
{
    return g_volume;
}

}  // namespace CardenzaHAL::Audio::Codec::ES8156
