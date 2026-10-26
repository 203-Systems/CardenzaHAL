# Cardenza HAL

[English](README.md) | [简体中文](README.zh-CN.md)

基于 ESP-IDF 的 Cardenza（ESP32-S3）硬件驱动：音频、显示屏、键盘及独立按键灯、SD 存储、PDM 麦克风、红外、Func 按键和 I2C。

## 项目接入

```cmake
add_subdirectory(path/to/cardenza)
target_link_libraries(your_target PRIVATE CardenzaHAL)
```

头文件路径以 HAL 根目录为起点。直接将驱动源码编入 ESP-IDF 组件时，将 HAL 根目录加入 `INCLUDE_DIRS`。

## 音频

```cpp
#include "cardenzaHAL.h"

// 在 CardenzaHAL::Init() 之后、启用 TX 之前选择模式：
CardenzaHAL::Audio::Init(I2S_SLOT_MODE_STEREO);
auto tx = CardenzaHAL::Audio::GetTxChannel();
```

确认初始化成功后，用 `i2s_channel_enable(tx)` 启用 TX。默认单声道：每帧一个 `int16_t`，由硬件复制到左右输出。双声道接收交错的 `L0, R0, L1, R1, ...` 采样。两种模式均使用 44.1 kHz、16-bit PCM 和 32-bit I2S slot。切换模式前禁用 TX。音频渲染和 PCM 转换由应用负责。

## 键盘灯

键盘初始化后即可调用：

```cpp
namespace Keys = CardenzaHAL::Keyboard;
Keys::setAllLEDs(8);
Keys::setLED({0, 0}, 15);
Keys::setLEDGlobalBrightness(10);
Keys::enableLED(true);
```

每键和全局亮度均为 **0–15**。每键底层严格 **16 档**；全局亮度通过 LED_EN 独立硬件 PWM 调节，固定采用近似 gamma 2 的补偿，不修改每键数值。灯光更新自动生效，由同一个控制任务调用。

LED 引脚固定为 **EN47 / CLK48 / DAT21**。键盘 PWM 使用 LEDC timer 1 / channel 6，屏幕背光使用 timer 0 / channel 7，两者统一使用 APB。键盘事件返回 14 × 4 矩阵中的 `{x, y}` 坐标，应用自行定义按键映射。

## Launcher 应用

应用元数据和登记命令见 [203-launcher 应用登记指南](https://github.com/203-Systems/203-launcher/blob/main/docs/REGISTERED_SOFTWARE.md#developer-workflow)。
