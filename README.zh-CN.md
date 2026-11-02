# Cardenza HAL

[English](README.md) | [简体中文](README.zh-CN.md)

基于 ESP-IDF 的 Cardenza（ESP32-S3）硬件驱动：音频、显示屏、键盘及独立按键灯、SD 存储、PDM 麦克风、红外、Func 按键、I2C 和 USB（MIDI、音频、HID）。

## 项目接入

HAL 是一个 ESP-IDF 组件。把它的目录加进 `EXTRA_COMPONENT_DIRS`，按目录名依赖它；它自带依赖（TinyUSB，见 `idf_component.yml`）：

```cmake
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/cardenza")   # 工程的 CMakeLists.txt
idf_component_register(... REQUIRES cardenza)                     # 你的组件
```

头文件路径以 HAL 根目录为起点（`"cardenzaHAL.h"`、`"usb/usb.h"` 等）。不用 ESP-IDF 组件时，仍可 `add_subdirectory` 加 `target_link_libraries(your_target PRIVATE CardenzaHAL)` 使用 USB 以外的驱动。

## USB

`CardenzaHAL::USB::Init()`（`usb/usb.h`）把 USB 口变成一个复合设备：USB MIDI 1.0、USB 音频 2.0（立体声 16 位 44.1 kHz，双向，异步带反馈：电脑跟随设备的时钟）和 HID（键盘、媒体键、系统通道和固件自己的原始报告）。VID 0x0203（203 Systems）。报告号和指令沿用 Matrix OS：报告 0xCB 是系统指令（0x40 Launcher / 更新模式，0x41 重启，0x42 ROM 下载模式，0x43 控制台，0x44 识别；应答为指令 | 0x80），同样的指令也可以用 SysEx `F0 23 <指令> F7` 发。

- 它占用芯片唯一的 USB PHY：控制台转到系统通道，esptool 的串口没有了，直到重启进 Launcher 或 ROM 下载模式。`Init` 时按住 Func 则保留 USB-Serial-JTAG。
- ESP32-S3 除 EP0 外只有四个 IN 端点，这个设备全部用上（MIDI、音频输入、音频反馈、HID），放不下 CDC。
- TinyUSB 只由 `usb/config/tusb_config.h` 配置。同一个固件里不要再加 `esp_tinyusb` 或其他配置 TinyUSB 的东西。
- 内存：静态约 8.5 KB（音频 FIFO 每个方向 2 KB），启动后再加 4 KB 任务栈。
- `keyboard = true` 时，macOS 会请用户识别一次键盘，电脑上的工具也只有在"输入监控"权限下才能打开 HID 接口（包括系统通道）。SysEx 不需要权限。
- 已在 macOS 上实测（2026-11-02）：MIDI 双向、HID 和 SysEx 系统指令、控制台、44.1 kHz 双向音频。全速下反馈值按 10.14 发 3 字节（TinyUSB 默认的 16.16 会让 macOS 在 128 ms 后停播）；请求 Microsoft OS 字符串的主机收到 16.16（Windows，未测试）。TinyUSB 的 DWC2 驱动不会重启错过帧的同步 OUT 传输；HAL 在 20 ms 收不到数据时重启播放端点。

## Launcher 应用

应用元数据和登记命令见 [203-launcher 应用登记指南](https://github.com/203-Systems/203-launcher/blob/main/docs/REGISTERED_SOFTWARE.md#developer-workflow)。
