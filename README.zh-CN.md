# Cardenza HAL

[English](README.md) | [简体中文](README.zh-CN.md)

基于 ESP-IDF 的 Cardenza（ESP32-S3）硬件驱动：音频、显示屏、键盘及独立按键灯、SD 存储、PDM 麦克风、红外、Func 按键和 I2C。

## 项目接入

```cmake
add_subdirectory(path/to/cardenza)
target_link_libraries(your_target PRIVATE CardenzaHAL)
```

头文件路径以 HAL 根目录为起点。直接将驱动源码编入 ESP-IDF 组件时，将 HAL 根目录加入 `INCLUDE_DIRS`。

## Launcher 应用

应用元数据和登记命令见 [203-launcher 应用登记指南](https://github.com/203-Systems/203-launcher/blob/main/docs/REGISTERED_SOFTWARE.md#developer-workflow)。
