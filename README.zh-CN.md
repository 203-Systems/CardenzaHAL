# Cardenza HAL

[English](README.md) | [简体中文](README.zh-CN.md)

基于 ESP-IDF 的 Cardenza（ESP32-S3）硬件抽象层。

提供音频输出（I2S + ES8156）、显示屏、14 × 4 键盘及独立按键灯、SD 存储、PDM 麦克风、红外、Func 按键和共享 I2C 总线驱动。

## 项目接入

将本仓库作为子模块加入 ESP-IDF 项目，并链接 HAL：

```cmake
add_subdirectory(path/to/cardenza)
target_link_libraries(your_target PRIVATE CardenzaHAL)
```

头文件路径以 HAL 根目录为起点：

```cpp
#include "cardenzaHAL.h"
#include "audio/audio.h"
```

直接将驱动源码编入 ESP-IDF 组件的项目，应将 HAL 根目录加入 `INCLUDE_DIRS`。

## 音频配置

如果使用 `CardenzaHAL::Init()`，先完成整体初始化，再于启用 TX 前选择声道模式：

```cpp
CardenzaHAL::Audio::Init();                     // 默认单声道
// 或：CardenzaHAL::Audio::Init(I2S_SLOT_MODE_STEREO);
auto tx = CardenzaHAL::Audio::GetTxChannel();
```

确认 `Init()` 成功后，使用 `i2s_channel_enable(tx)` 启用通道。单声道每帧接收一个 `int16_t`，由硬件复制到左右输出。双声道接收交错的 `int16_t` 采样（`L0, R0, L1, R1, ...`）；单声道合成器采用双声道模式时，需要自行复制采样。`GetTxChannel()` 保留已选择的模式。

重复初始化相同模式不会重新配置 TX。切换模式前需要先禁用 TX，播放中切换返回 `false`。初始化调用应由同一个设置或控制任务执行。

## 203-launcher 应用

将 Cardenza 应用加入 [203-launcher](https://github.com/203-Systems/203-launcher) 时，请参阅[应用登记指南](https://github.com/203-Systems/203-launcher/blob/main/docs/REGISTERED_SOFTWARE.md#developer-workflow)，了解元数据格式和登记命令。裸 BIN 导入后会显示为 Unknown App，需要显式登记应用身份。

## 使用说明

- 需要 ESP-IDF，不支持桌面构建。
- 键盘使用 `{x, y}` 矩阵坐标，应用自行定义按键映射。
- 音频默认采用 44.1 kHz、16-bit 单声道 PCM，I2S slot 宽度为 32 bit（64fs）。slot 宽度与声道数是独立设置。两种模式都会使用左右两个传输 slot。通过原生 ESP-IDF API 修改 slot 宽度时，也需要同步匹配 ES8156 时钟配置。
