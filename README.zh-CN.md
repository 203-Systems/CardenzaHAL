# Cardenza HAL

[English](README.md) | [简体中文](README.zh-CN.md)

CardenzaHAL 是 Cardenza（ESP32-S3）的 ESP-IDF 硬件层。你可以用它读取 56 键键盘、控制每颗按键灯、向 240 × 135 屏幕绘图、播放 PCM 音频、访问 SD 卡，给固件加入 USB MIDI、音频和 HID 功能，以及通过 203 Launcher 设置好的 Wi-Fi 上网。

HAL 负责硬件初始化，并提供原生 ESP-IDF 句柄；应用负责界面绘制、键位映射、音频处理、文件格式和主循环。本仓库是组件，需要接入固件工程使用；它的网页目标还能把你的固件编译成浏览器里的模拟器（见[网页模拟器](#网页模拟器)）。

## 使用前提

- 当前版本的 Cardenza 硬件，引脚定义见 [`board/pins.h`](board/pins.h)。
- **ESP-IDF 5.3 或更新版本**，已安装 ESP32-S3 工具链并激活 IDF 环境。这是依赖清单声明的最低版本，不代表所有后续版本都已通过兼容测试。
- C++17 或更新标准，使用 `.cpp` 文件引用 HAL 头文件。
- 启用 ESP-IDF Component Manager。[`idf_component.yml`](idf_component.yml) 会引入 `espressif/tinyusb` `~0.21.0`。

驱动依赖 ESP-IDF。备用的 CMake 目标 `CardenzaHAL` 同样需要嵌入式 ESP-IDF 环境，不能用来直接编译桌面程序或模拟器。

## 快速上手

下面的完整示例会点亮屏幕，并让按键灯跟随按下状态变化。只有显式初始化 USB 设备模块后，USB 才会从 USB-Serial-JTAG 切换到复合设备。

### 1. 加入组件

在你的固件工程根目录执行：

```sh
mkdir -p components
git submodule add https://github.com/203-Systems/CardenzaHAL.git components/CardenzaHAL
```

如果固件已经包含该子模块，克隆后执行 `git submodule update --init --recursive` 即可。如果固件还不是 Git 仓库，可以改用 `git clone https://github.com/203-Systems/CardenzaHAL.git components/CardenzaHAL`。

工程结构如下：

```text
my-firmware/
├── CMakeLists.txt
├── components/
│   └── CardenzaHAL/
└── main/
    ├── CMakeLists.txt
    └── main.cpp
```

工程根目录的 `CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.20)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cardenza_hello)
```

`main/CMakeLists.txt`：

```cmake
idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "." REQUIRES CardenzaHAL)
```

ESP-IDF 会自动发现 `components/` 下的组件。如果把 HAL 放在其他目录，在包含 `project.cmake` **之前**设置 `EXTRA_COMPONENT_DIRS`。`REQUIRES` 必须使用 HAL 的目录名：目录叫 `cardenza` 就写 `REQUIRES cardenza`。头文件仍从 HAL 根目录引用，例如 `"cardenzaHAL.h"`。

### 2. 初始化并读取按键

`main/main.cpp`：

```cpp
#include "cardenzaHAL.h"
#include <cstdint>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern "C" void app_main()
{
    if (!CardenzaHAL::Init()) {
        const auto& r = CardenzaHAL::LastInitReport();
        ESP_LOGE("app", "HAL init failed: system=%d keyboard=%d audio=%d display=%d",
                 r.system, r.keyboard, r.audio, r.display);
        return;
    }

    namespace Display = CardenzaHAL::Display;
    namespace Keyboard = CardenzaHAL::Keyboard;

    // Static storage keeps this 64,800-byte buffer off the task stack.
    static uint16_t framebuffer[Display::WIDTH * Display::HEIGHT] = {};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < Display::WIDTH; ++x)
            framebuffer[y * Display::WIDTH + x] = 0xFFFF;

    if (!Display::SetBrightness(160) || !Display::Present(framebuffer)) {
        ESP_LOGE("app", "Display submission failed");
        return;
    }

    Keyboard::setLEDGlobalBrightness(8);
    Keyboard::enableLED(true);  // Key LEDs start disabled.
    while (true) {
        Keyboard::updateKeyList();
        Keyboard::LedFrame leds{};
        for (const auto& key : Keyboard::keyList())
            leds[key.y * Keyboard::LED_COLUMNS + key.x] = 15;
        Keyboard::setLEDFrame(leds);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

示例使用静态数组存放 64,800 字节的帧缓冲，避免占用任务栈。按键灯默认关闭，必须先调用 `enableLED(true)`。

### 3. 编译并运行固件

激活 ESP-IDF 环境后，在固件根目录执行：

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

把 `<PORT>` 换成实际设备端口。运行后，黑色屏幕顶部出现一条白色横条；按住哪个键，哪个键就亮，松开后熄灭。没有 SD 卡也可以运行。示例初始化了音频硬件，但不会播放声音。

这些命令用于你的固件工程。通过 Launcher 分发的应用还需要匹配的分区布局和元数据，见 [Launcher 应用](#launcher-应用)。

## 初始化与错误处理

[`CardenzaHAL::Init()`](cardenzaHAL.cpp) 按下表顺序初始化：

| 模块 | 是否影响 `Init()` 成功 | 行为 |
| --- | --- | --- |
| System | 是 | 初始化共享 I2C 总线和 Func；失败则立即返回 |
| Keyboard | 是 | 启动后台按键与 LED 扫描 |
| Audio | 是 | 配置单声道 TX 并启动 ES8156；新建的 TX 通道保持禁用 |
| Display | 是 | 准备屏幕，提交一帧后才显示 |
| Storage | 否 | 尝试挂载 `/sdcard` |
| IR | 否 | 准备红外发射器 |

`LastInitReport()` 提供上述六个模块的结果和 `overall`。整体初始化成功后会缓存结果，再调用一次不会重新挂载缺失的 SD 卡；需要单独调用 `Storage::Init()` 重试。麦克风、USB 和网络（[Net](#网络wi-fi-与-https)）**不在整体初始化范围内**；`cardenzaHAL.h` 也不包含 `usb/usb.h` 和 `net/net.h`。打开 `CONFIG_CARDENZA_HAL_PRESENCE`（默认打开）时，`Init()` 还会启动一次[社区在线服务](#社区在线服务任何固件运行时都能收到)，不论各模块的结果如何。

只需要部分功能时，可以分别初始化各模块。使用共享 I2C 或 Func 前，先调用 `System::Init()`，并检查各模块的返回值。音频编解码器初始化失败后可以再次调用 `Audio::Init()`，重试频率限制约为每秒一次。

## 键盘与按键灯

接口：[`keyboard/keyboard.h`](keyboard/keyboard.h)、[`keyboard/types.h`](keyboard/types.h)。

- 坐标范围为 `x = 0..13`、`y = 0..3`。按键编号为 **1..56**，公式是 `y * 14 + x + 1`。字符映射由应用定义。
- 读取 `keyList()`、`isPressed()`（当前按下数量）、`isKeyPressing(number)` 前，先调用 `updateKeyList()`。把返回的列表当作只读快照；需要保留时自行复制。
- `getKey()` 会更新快照并返回第一个按住的坐标，无按键时返回 `{-1, -1}`。`getKeyNum()` 对负坐标返回 `0`，其他情况只应传有效坐标。
- `isChanged()` **只比较按键数量**。同时松开一个键、按下另一个键时，数量可能不变。需要按下/松开事件时，比较前后两次完整快照。
- `KEY_PRESS_LIMIT` 为 **4**，包括修饰键。采样到超过四个按键时，会阻止新按键进入，直到它们稳定释放；已经确认的按键仍能正常释放。不要按无限多键无冲来设计交互。

所有 LED 亮度值都是 **0..15**，超出上限会截断为 15。先用 `enableLED(true)` 开启输出，再用 `setLED({x, y}, level)`、`setAllLEDs(level)` 或 `clearAllLEDs()` 设置亮度。`enableLED(false)` 会保留各键亮度；返回值是最终启用状态，因此成功关闭时也会返回 `false`。

做动画时，先填充 `LedFrame`，索引为 `y * LED_COLUMNS + x`，再调用一次 `setLEDFrame()` 发布。全局亮度不会修改已保存的单键亮度：`setLEDGlobalBrightness(0)` 全灭，`15` 为最大值。单键使用四个位平面控制，全局使用固定 gamma-2 PWM。按键快照读取和 LED 控制统一放在一个应用控制任务中。

驱动之上有两个可选的辅助头文件。它们都是纯头文件，电脑上的模拟器和测试也能用：

- [`keyboard/led_light.h`](keyboard/led_light.h)：单键等级是线性占空比，人眼看起来并不线性（7 级看着远比一半亮）。`Light::PerceivedToLevel(perceived)` 把 0..15 的感知亮度换成要发送的等级（gamma 2.6，在板上目测定的；亮着的等级不会变灭），`Light::LightPermille(perceived, global)` 给出这个键实际发出的光，供模拟器绘制。
- [`keyboard/key_settle.h`](keyboard/key_settle.h)：`KeySettle<N>` 给读错了代价最大的键（修饰键、导航键）再加一道过滤：状态保持 12 ms 才算数；读成按住超过 15 秒的键算作已松开，直到它真的松开（两个时间都可设）。每帧把各键的读数喂给它；`Glitches()` 和 `Stuck()` 统计过滤掉了什么，可写进日志。

## 屏幕显示

接口：[`display/display.h`](display/display.h)。

- 横屏 **240 × 135**，RGB565；帧缓冲包含 `WIDTH * HEIGHT` 个 16 位像素，共 **64,800 字节**，行跨度为 `WIDTH`。面板配置为小端 RGB 数据。
- `Present(framebuffer)` 同步提交全屏。帧缓冲可以在 PSRAM 中，HAL 使用内部 DMA 小块转发。调用返回后即可复用调用方的帧缓冲，失败时也一样。
- 局部刷新时，先修改完整帧缓冲，再提交它里面的矩形区域：

```cpp
const CardenzaHAL::Display::Region dirty{0, 0, 40, 16};
bool ok = CardenzaHAL::Display::Present(framebuffer, &dirty, 1);
```

矩形宽高必须大于零，且完全位于屏幕内。首次提交及提交失败后的恢复会强制刷全屏；首次全屏完成后，传零个矩形表示没有像素变化。

`SetBrightness(0..255)` 接收感知亮度，HAL 已应用 gamma 2.2，无需重复校正。首次 `Present()` 成功前只记录亮度请求。冷启动会重置面板，软件重启会保留旧画面直到提交新帧。

使用原生渲染接口时，获取 `GetPanel()` / `GetPanelIO()` 句柄，完成绘制提交后调用无参数的 `Present()`，等待排队绘制完成并显示画面。不要把自定义 panel-IO 回调与帧缓冲版 `Present()` 混用。屏幕提交和控制应由同一任务调用。

整屏一次是 17 ms 的 SPI，占了 50 fps 一帧的大半。**`PresentChanged(framebuffer)`** 自己算出哪里变了，只发那些：每行按 30 像素一段算哈希，每条 8 行的带子只发变了的行、横向覆盖变了的段；每帧另外整条重发一带，约 0.34 秒整屏刷新一遍。帧缓冲要 4 字节对齐（`alignas(4)`）；4.3 KB 的哈希表第一次用时才申请（有 PSRAM 就放 PSRAM），申请不到就整帧发送。它和其他 `Present` 二选一，不要混用。计算部分是 [`display/changed_regions.h`](display/changed_regions.h)，纯头文件，主机测试或自己负责发送的固件也能用。

## 音频输出

接口：[`audio/audio.h`](audio/audio.h)。

ES8156 输出采样率为 **44,100 Hz**，应用提供有符号 **16 位 PCM**，I2S 线上使用 **32 位时隙**。`Audio::Init()` 默认单声道，每帧一个采样复制到左右两个时隙；`Audio::Init(I2S_SLOT_MODE_STEREO)` 接收左右交错采样。切换声道模式前必须禁用原生 TX 通道。

初始化后，由应用启用通道并写入 PCM。下面的函数会播放约一秒、较低电平的 440 Hz 音调，在 HAL 成功初始化后调用：

```cpp
#include "audio/audio.h"
#include <cmath>
#include <cstdint>
#include <esp_err.h>

void PlayTone()
{
    namespace Audio = CardenzaHAL::Audio;
    if (!Audio::Init(I2S_SLOT_MODE_STEREO) || !Audio::SetVolume(30)) return;
    auto tx = Audio::GetTxChannel();
    ESP_ERROR_CHECK(i2s_channel_enable(tx));

    int16_t pcm[128 * 2];
    int sampleIndex = 0;
    for (int block = 0; block < 345; ++block) {
        for (int frame = 0; frame < 128; ++frame, ++sampleIndex) {
            const auto sample = static_cast<int16_t>(
                2000.0 * std::sin(6.283185307179586 * 440.0 * sampleIndex / 44100.0));
            pcm[frame * 2] = sample;
            pcm[frame * 2 + 1] = sample;
        }
        size_t written = 0;
        ESP_ERROR_CHECK(i2s_channel_write(tx, pcm, sizeof(pcm), &written, 1000));
        if (written != sizeof(pcm)) break;
    }
    ESP_ERROR_CHECK(i2s_channel_disable(tx));
}
```

`i2s_channel_write()` 的长度单位是**字节**，超时单位是**毫秒**。实际音频任务应处理部分写入并连续供给音频块。采样生成、混音、PCM 转换和调度都由应用负责。

`SetVolume(0..100)` 会缓存请求值，只有编解码器就绪时才写入硬件；未就绪或 I2C 写入失败时返回 `false`，不会自行初始化编解码器。`GetVolume()` 返回请求值，`IsCodecReady()` 返回编解码器状态。

## 麦克风与音频共用引脚

接口：[`mic/mic.h`](mic/mic.h)。

PDM 输入使用 I2S0，输出为 **44,100 Hz、单声道、有符号 16 位 PCM**。音频输出使用 I2S1，但麦克风 CLK 与扬声器 WS 共用 **GPIO43**。应用必须协调引脚归属，不能让两个接口在该引脚上各自同时运行。

从播放切换到录音：

1. 停止音频写入任务，用 `i2s_channel_disable()` 禁用 TX。
2. 调用 `Audio::StandbyCodec()` 并确认成功。
3. 调用 `Mic::Init()`，获取 `Mic::InputChannel()`，检查初始化结果和句柄。
4. 在 RX 禁用时调用 `Mic::RestoreInputPins()`；重新使用已存在的麦克风通道时也需要这一步。
5. 用 `i2s_channel_enable()` 启用 RX，然后使用 `i2s_channel_read()` 读取。

恢复播放时，先停止读取并禁用 RX，再依次调用 `Audio::RestoreOutputPins()`、所需声道模式的 `Audio::Init()`，最后启用 TX；每步都检查结果。`InputChannel()` 可能初始化 RX，但不会启用它。HAL 不提供录音启停/读取封装，也不会自动仲裁引脚。

## SD 卡存储

接口：[`storage/storage.h`](storage/storage.h)。

`Storage::Init()` 通过 4-bit SDMMC 把 FAT 文件系统挂到 **`/sdcard`**，最多同时打开五个文件；挂载失败时**不会格式化**。确认 `IsMounted()` 后，可使用标准 C/C++ 文件接口：

```cpp
#include "storage/storage.h"
#include <cstdio>

void ReadSettings()
{
    if (!CardenzaHAL::Storage::Init()) return;
    FILE* file = std::fopen("/sdcard/settings.txt", "r");
    if (!file) return;
    char line[128];
    while (std::fgets(line, sizeof(line), file)) {
        // Parse a line in your application.
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) CardenzaHAL::Storage::CheckAfterIoError();
}
```

`GetCapacity()` 返回以**字节**为单位的总容量，不是剩余空间。`IsMounted()` 是缓存状态，不是硬件插卡检测。I/O 出错后，先关闭受影响文件，再调用 `CheckAfterIoError()`：卡仍正常响应时保持挂载（例如只是写满），失去响应时标记为不可用，并在下一次 `Init()` 时清理旧挂载。重试前须协调所有文件使用者；HAL 不提供自动热插拔管理。

## Func、I2C 与红外

- [`System::GetFuncButton()`](system/system.h) 在 **GPIO0 的 Func 按键**被按下时返回 `true`。它是独立按键，与矩阵键盘及其中的 Fn 键分开。
- `System::GetI2CBus()` 返回 `System::Init()` 创建的原生 I2C 主总线句柄。用 `i2c_master_bus_add_device()` 添加设备，避免在同一组引脚重复创建总线（SCL GPIO1、SDA GPIO2）。
- `System::EnterDeepSleep()` 当前**只删除共享 I2C 总线**，并不会让 ESP32 进入深度睡眠，也不会配置唤醒源。调用前需要协调编解码器和其他 I2C 使用者。
- [`IR::Send(addr, cmd)`](ir/ir.h) 在 GPIO44 上用 38 kHz 载波发出 8 位 NEC 地址/命令及各自反码。必要时会自行初始化发射器，并等待发送结束。没有接收接口，也不返回发送状态。

## USB 设备用法

单独包含 `usb/usb.h`，在系统初始化之后启动 USB。默认配置包含 MIDI、音频、HID 键盘、媒体键和系统通道。只需要 MIDI 应用接口时，可以这样配置：

```cpp
#include "usb/usb.h"

CardenzaHAL::USB::Config config;
config.product = "My Cardenza App";
config.audio = false;
config.keyboard = false;
config.consumer = false;
// config.fallbackSeconds = 30;  // Optional development recovery; leave 0 in products.
bool started = CardenzaHAL::USB::Init(config);
```

系统 HID 通道仍然保留。USB 已启动、开发恢复机制跳过本次启动，或启动失败时，`Init()` 都可能返回 `false`。可分别检查 `Started()` 和 `Mounted()`；后者表示已被主机配置且未挂起。

| 接口 | 数据与应用责任 |
| --- | --- |
| `MidiRead(packet)` / `MidiWrite(packet)` | 虚拟线缆 0 的四字节 USB-MIDI 1.0 事件包，不是三字节原始 MIDI 流。例如 `{0x09, 0x90, 60, 100}` 表示通道 1 Note On；检查返回值。 |
| `AudioWrite(buffer, frames)` | 44.1 kHz、左右交错的立体声 `int16_t`，发给主机录音 |
| `AudioRead(buffer, frames)` | 读取主机播放流，返回实际读取的立体声帧数 |
| `KeyboardReport(modifiers, keys)` | 六个 HID 键码与修饰键位；发送全零报告以释放按键 |
| `ConsumerReport(usage)` | 一个媒体功能 usage，`0` 表示释放 |
| `SetAppReportHandler(handler)` / `SendAppReport(data, length)` | 最多 `kReportSize`（63）字节的应用报告；回调在 USB 任务中执行，应保持简短 |

USB 音频长度单位是**帧**，每帧两个采样，共四字节。每个音频块都应服务收发并处理短传输。主机没有录音时，`AudioWrite()` 会丢弃数据但仍返回请求帧数；需要区分时检查 `AudioInputActive()`。`AudioOutputActive()` 表示主机是否正在播放。

USB 音频不会自动接到扬声器或麦克风，应用需自行桥接，包括声道转换及共用引脚管理。需要响应主机音量时，由应用读取并应用 `HostMuted()`、`HostVolume()`；后者单位是 1/256 dB，`0` 表示满电平。`AudioStats()` 提供端点计数和以字节为单位的 FIFO 占用量。

### USB 协议与平台说明

`CardenzaHAL::USB::Init()`（`usb/usb.h`）把 USB 口变成一个复合设备：USB MIDI 1.0、USB 音频 2.0（立体声 16 位 44.1 kHz，双向，异步带反馈：电脑跟随设备的时钟）和 HID（键盘、媒体键、系统通道和固件自己的原始报告）。VID 0x0203（203 Systems）。报告号和指令沿用 Matrix OS：报告 0xCB 是系统指令（0x40 Launcher / 更新模式，0x41 重启，0x42 ROM 下载模式，0x43 控制台，0x44 识别；应答为指令 | 0x80），同样的指令也可以用 SysEx `F0 23 <指令> F7` 发。固件自己的报告（0xFF）在电脑没法用 HID 时（比如用 Web MIDI 的网页）也能用 SysEx `F0 23 7F <字节> F7` 发，`SendAppReport` 按上一条来的方式回复（`F0 24 7F <字节> F7`，7 位）。固件的 MIDI 包和 HAL 的 SysEx 一次只有一方在写，SysEx 到电脑时是完整的。

- 它占用芯片唯一的 USB PHY：控制台转到系统通道，esptool 的串口没有了，直到重启进 Launcher 或 ROM 下载模式。
- ESP32-S3 除 EP0 外只有四个 IN 端点，这个设备全部用上（MIDI、音频输入、音频反馈、HID），放不下 CDC。
- TinyUSB 只由 `usb/config/tusb_config.h` 配置。同一个固件里不要再加 `esp_tinyusb` 或其他配置 TinyUSB 的东西。
- 内存：静态约 8.5 KB（音频 FIFO 每个方向 2 KB），启动后再加 4 KB 任务栈。
- `keyboard = true` 时，macOS 会请用户识别一次键盘，电脑上的工具也只有在"输入监控"权限下才能打开 HID 接口（包括系统通道）。SysEx 不需要权限。
- 已在 macOS 上实测（2026-11-02）：MIDI 双向、HID 和 SysEx 系统指令、控制台、44.1 kHz 双向音频。全速下反馈值按 10.14 发 3 字节（TinyUSB 默认的 16.16 会让 macOS 在 128 ms 后停播）；请求 Microsoft OS 字符串的主机收到 16.16（Windows，未测试）。TinyUSB 的 DWC2 驱动不会重启错过帧的同步 OUT 传输；HAL 在 20 ms 收不到数据时重启播放端点。

## 网络（Wi-Fi 与 HTTPS）

公开接口：[`net/net.h`](net/net.h)；纯头文件（不依赖 ESP-IDF，电脑上也能用）[`net/launcher_settings.h`](net/launcher_settings.h)、[`net/http_request.h`](net/http_request.h) 和 [`net/connect_order.h`](net/connect_order.h)。

给偶尔需要上网的固件用（分享一段录音、查点东西），用的是 203 Launcher 设置好的 Wi-Fi 网络和社区账号。不调用就什么都不运行（[在线服务](#社区在线服务任何固件运行时都能收到)打开时会自己使用 Net）：`ReadLauncherSettings` 读出 Launcher 保存的内容，`Connect` 打开 Wi-Fi，`Send` 发 HTTP(S) 请求，`Disconnect` 把一切关掉、把内存还回去。

```cpp
#include "net/net.h"
#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace Net = CardenzaHAL::Net;

// 单独一个任务：每个调用都会阻塞几秒。
void WhoAmITask(void*)
{
    static Net::LauncherSettings settings;  // 1.6 KB：不放在栈上
    Net::ConnectReport report;
    if (Net::ReadLauncherSettings(settings) == Net::SettingsResult::Ok && settings.account.Bound() &&
        Net::Connect(settings, report) == Net::ConnectStatus::Connected)
    {
        settings.Forget();  // Wi-Fi 密码用完了
        char url[160], auth[96];
        std::snprintf(url, sizeof(url), "%s/me", settings.account.api);
        std::snprintf(auth, sizeof(auth), "Bearer %s", settings.account.token);
        const Net::Header headers[] = {{"Authorization", auth}, {"Accept", "application/json"}};
        Net::Request request;
        request.url = url;
        request.headers = headers;
        request.headerCount = 2;
        static char body[2048];
        Net::Response response;
        response.body = body;
        response.capacity = sizeof(body);
        if (Net::Send(request, response) == Net::SendStatus::Ok && response.status == 200) {
            // body 里是 JSON 回复（response.truncated：原本更长）
        }
        Net::Disconnect();
    }
    vTaskDelete(nullptr);
}
// xTaskCreatePinnedToCore(WhoAmITask, "net", 8192, nullptr, 2, nullptr, 0);
```

文件从它的文件描述符一块一块地发（`kUploadBlockBytes`，4 KB，读进内部 RAM，因为 SD 卡的 DMA 够不到 PSRAM），每块之后报告进度：

```cpp
const int fd = open("/sdcard/take.wav", O_RDONLY);
struct stat info;
fstat(fd, &info);
const Net::Header headers[] = {{"Content-Type", "audio/wav"}};
Net::Request request;
request.method = "PUT";
request.url = signedUploadUrl;
request.headers = headers;
request.headerCount = 1;
request.bodyFd = fd;
request.bodyFileLength = info.st_size;
Net::Response response;  // 不给缓冲区：只要状态码
Net::Send(request, response, [](void*, uint64_t sent, uint64_t total) {
    // 显示 sent / total；返回 false 就停止
    return true;
});
close(fd);
```

| 调用 | 作用 |
| --- | --- |
| `ReadLauncherSettings(settings)` | 读出 Launcher 的账号、保存的网络（密码已解密）和上次连接的网络。返回 `Ok`、`NoPartition`（不是 Launcher 的分区表）或 `Unreadable`（见下文）。不需要 Wi-Fi。 |
| `Connect(settings, report, timeoutMs = 20000, cancelled, context)` | 先连 Launcher 上次连过的网络，在它的信道上直接连、不扫描；不行就扫描（约 1.5 秒），按信号从强到弱试范围内保存过的网络（每个最多 15 秒，期间重连两次）。返回 `Connected`、`NoNetworks`、`NotFound`、`AuthFailed`（密码被拒，或握手超时）、`Failed`、`NoMemory`、`Cancelled`；`report` 里有网络名、信号、耗时和错误码。失败时把打开的东西全部关掉。 |
| `Connected()` | 拿到了 IP 地址。连接断了不会自动重连：再调用 `Connect`。 |
| `Disconnect()` | 停止并反初始化 Wi-Fi，删掉它的 netif，以及（如果是 Net 建的）默认事件循环。在线服务占用着连接时，连接为它保留（固件只是放手，省电模式回到调制解调器睡眠）。 |
| `Send(request, response, progress, cancelled, context)` | 一个请求，每次新建连接。`https` 用 ESP-IDF 的证书包校验；不跟随重定向。回复正文写进调用者的缓冲区（以零结尾，更长时 `truncated`）。`Ok` 表示收到了回复，不论 HTTP 状态码是多少。 |
| `RunningImageDigest(hex)` | 正在运行的镜像末尾附带的 SHA-256，社区用它认出一个构建（64 位十六进制，即 `esp_partition_get_sha256` 的结果）。只读镜像的各个头部就能找到（第一次调用时十几次小的闪存读取，结果会保存下来）：bootloader 启动它时已经校验过镜像。 |

`ValidRequest`（`net/http_request.h`）是 `Send` 先做的检查：方法是 GET、POST、PUT、DELETE、HEAD 或 PATCH；`http://` 或 `https://` 的 URL，没有用户信息和片段，路径和查询是可打印 ASCII；头的名字是 token，值里没有控制字符（没有 CR 或 LF）；不能有 `Host`、`Content-Length`、`Transfer-Encoding`、`Connection` 头（由 Net 设置）；正文来自内存或文件，不能两者都给，最多 `INT_MAX` 字节。

**线程。** 在固件自己的一个任务里调用 Net，一次一个调用，绝不在界面或音频任务里调用。给它 6-8 KB 的栈（TLS 握手在上面运行；[`examples/launcher_net`](examples/launcher_net) 会打印实际用了多少），优先级低于界面。连接时大约每 100 ms 检查一次 `cancelled`，上传时每块之间检查一次；单个网络操作（建立连接、一次读或写）仍可能阻塞到 `Request::timeoutMs`。按下面的配置，Wi-Fi 任务（优先级 23）、lwIP 任务（18）和事件循环任务（20，总在核 0）都在核 0 上运行。固件自己已经在用 Wi-Fi 时，Net 拒绝启动 Wi-Fi。

**Launcher 的设置**保存在设备上所有固件共用的 NVS 分区里（`nvs`，0x9000）。Net 读 Launcher 的三个命名空间，**从不往里写任何东西**（Net 唯一一次写共享 NVS 是在线服务交接时写 `l_pending`，见下文）：

| 命名空间 | 键 | Net 中的对应 |
| --- | --- | --- |
| `l_account` | `token`（`ccd_` + 64 位十六进制）、`api`（社区 API 地址，`https://community.203.io/api`）、`user_id`、`user_name`、`device_id`（芯片的 12 位十六进制 MAC） | `LauncherAccount`；和 Launcher 一样，token 不合法或没有 api 的记录不算绑定（`Bound()` 为假）；超过 63 字节的名字在字符边界截断 |
| `l_wifi` | 每个保存的网络，用 SSID 的 CRC-32 作键：`s_<CRC>` 是 SSID，`p_<CRC>` 是密码（用所有 Launcher 共用的密钥和 IV 做 AES-128-CBC 加 PKCS#7 填充，再 base64：只是混淆，不是加密；开放网络为空），`b_<CRC>` = 1 | `networks[]`，最多 16 个；缺了某一项或解不开的条目和 Launcher 一样跳过 |
| `launcher` | `wifi_last`（上次连接的 SSID）、`wifi_ch`（它的信道）、`remote_install`（设置 › 账号 › 接收固件；0 为关，没有这一项为开） | `lastSsid`、`lastChannel`、`receiveFirmware` |

**这些对板子上的固件都不是秘密。** 设备运行的任何固件都能读到设备令牌，Wi-Fi 密码也只是混淆过：每个 Launcher 的密钥和 IV 都相同（`net/launcher_settings.h` 里也有），所以能在板子上运行固件、或者读出闪存的人都能拿到密码。正因为如此，社区把设备令牌设计成最小权限（只能访问这台设备自己的接口和公开页面，碰不到账号设置、其他凭据或个人数据）。真正的保护需要每台设备不同的密钥（eFuse HMAC 密钥，或闪存加密加 NVS 加密），Launcher 目前没有使用。

分区通过一份标成只读的分区表项副本打开：NVS 可能做的每次写入或擦除都经过 `esp_partition_write` / `write_raw` / `erase_range`，它们在碰到闪存之前就拒绝只读表项（`ESP_ERR_NOT_ALLOWED`），NVS 也不允许在上面打开读写句柄。ESP-IDF 的 NVS 在打开分区时，遇到几种状态会写入：空白分区（新建一页）、被断电打断的换页（把它做完）、写了一半的条目或被打断的写入留下的重复值、对不上的 blob 片段。在只读副本上，这时打开会失败，或者（其中几种）只在内存里丢掉受影响的条目；Net 把 NVS 统计的已写条目数和闪存上各页的实际数目对比，有出入就报告 `Unreadable`。它从不修复：Launcher 下次启动时会修好自己的分区。用更新的 NVS 格式写的分区、没有空闲页的分区也是 `Unreadable`。固件自己已经打开 `nvs` 时，Net 直接通过它读，并保持打开。Net 的其他部分不碰 NVS：Wi-Fi 以 `nvs_enable = 0` 和 `WIFI_STORAGE_RAM` 启动，固件关掉 PHY 的校准数据存储（见下文）。

**内存。**

- 链接进来时（固件只要调用了任何一个 Net 函数）：约 36 KB 静态内部 RAM（Wi-Fi、PHY 和 lwIP 的数据，以及即使关掉 IRAM 选项也必须留在 IRAM 里的 13 KB Wi-Fi、PHY 和系统代码）和约 610 KB 闪存，这是在一个已经运行 HAL 和 USB、使用下面这些选项的固件上测得的。从不调用 Net 的固件不受影响，各个内存段大小完全不变。打开 `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`（见下表）后，其中约 14 KB，即 Wi-Fi（`net80211`、`pp`）、lwIP 和 supplicant 的 `.bss`，会移到 PSRAM：CardenzaSynth 的静态内部 RAM 因此只增加 21.3 KB，而不是 34 KB（2026-11-17）。
- 连接期间：Wi-Fi 的接收缓冲区（6 个 1.6 KB 的静态缓冲区，从 `esp_wifi_init` 到 `esp_wifi_deinit`；动态缓冲区只在收发帧时占用），以及 Wi-Fi、lwIP 和事件任务：大约 40-60 KB 内部 RAM，实际数字由示例在板子上测出。
- 请求期间：esp_http_client 的缓冲区（1 KB，以及请求行）、上传块（4 KB，内部 RAM，只在发文件时）和 TCP 发送缓冲（上传时最多 16 KB），都在内部 RAM；TLS 30-50 KB（16 KB 接收和 4 KB 发送的记录缓冲、服务器证书、握手），在设置了 `CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC` 时放在 PSRAM。
- `Disconnect` 之后：lwIP 自己的任务会留下来（ESP-IDF 5.3 不能反初始化 lwIP：连同 3 KB 的栈约 4 KB），NVS 保留一个很小的管理对象；ESP-IDF 每次 `ReadLauncherSettings` 还会漏掉 8 字节（`nvs_flash_init_partition_ptr` 创建的分区对象）。
- PSRAM：按 [`examples/psram_heap`](examples/psram_heap) 的方式把 PSRAM 只给明确申请的代码（`CONFIG_SPIRAM_USE_MEMMAP`）时，Wi-Fi 和 lwIP 留在内部 RAM：ESP-IDF 的 `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` 需要 `SPIRAM_USE_CAPS_ALLOC` 或 `SPIRAM_USE_MALLOC`，而且申请的是 `MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM`，这种区域没有这个能力；`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` 的条件也一样。所以 mbedTLS 通过 Net 自己的分配器进 PSRAM：设置 `CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC` 后，Net 在第一次连接之前安装它（`mbedtls_platform_set_calloc_free`），优先 PSRAM，没有 PSRAM 时用内部 RAM；在这个模式下 ESP-IDF 自己只会让 mbedTLS 用普通的 `calloc`。ESP32-S3 的硬件 AES 和 SHA 可以用 PSRAM 里的缓冲区：它们的 DMA 能访问 PSRAM，ESP-IDF 的驱动会在前后回写缓存，或者经过内部 RAM 的小块中转。把 PSRAM 放进 malloc 的固件（`SPIRAM_USE_CAPS_ALLOC` / `SPIRAM_USE_MALLOC`）也可以改用 ESP-IDF 自带的选项。

**sdkconfig。** [`examples/launcher_net/sdkconfig.defaults`](examples/launcher_net/sdkconfig.defaults) 是参考配置，放在板子的默认配置之后：

| 选项 | 原因 |
| --- | --- |
| `CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=n` | 打开时，PHY 在 Wi-Fi 启动时从 NVS 命名空间 `phy` 读校准数据，完整校准后写回去：固件以读写方式打开了 NVS 时会写进 Launcher 的分区，没打开时每次启动都打印一行错误。关掉后每次都完整校准。 |
| `CONFIG_ESP_WIFI_NVS_ENABLED=n` | Wi-Fi 自己把设置存进 NVS 的默认值（Net 初始化时本来也会关掉）。 |
| `CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC=y` | 通过 Net 的分配器把 TLS 内存放进 PSRAM（见上文）。不设置的话，每个连接 mbedTLS 要占 30-50 KB 内部 RAM。 |
| `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`、`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN=y` | ESP-IDF 证书包里常用的一半（42 个根证书，17 KB）就有社区需要的：ISRG Root X1（Let's Encrypt，Vercel 给自己托管的域名用它，比如 `cardenza.203.io`）、GTS Root R1 和 GlobalSign Root CA（`*.vercel.app`）、GTS Root R4（`*.supabase.co`，社区的存储）。 |
| `CONFIG_ESP_WIFI_IRAM_OPT=n`、`CONFIG_ESP_WIFI_RX_IRAM_OPT=n`、`CONFIG_LWIP_IRAM_OPTIMIZATION=n` | IRAM 留给固件的热点代码（音频）。Wi-Fi 和 lwIP 在闪存里运行，吞吐量会低一些。 |
| `CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n`、`CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n` | 只作为站点连接用密码保护的网络：代码更少。 |
| `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=4`、`DYNAMIC_RX_BUFFER_NUM=16`、`DYNAMIC_TX_BUFFER_NUM=16`、`RX_BA_WIN=6` | Wi-Fi 的缓冲区在内部 RAM（每个 1.6 KB；静态 4 个，和 203 Launcher 一样）。块确认窗口不能超过动态接收缓冲区数，也不能超过静态接收缓冲区数的两倍。 |
| `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=16384`、`CONFIG_LWIP_TCP_WND_DEFAULT=5760` | 上传：TCP 在收到确认前能发出去的量决定了长往返时间下的速度（16 KB、往返 200 ms 约 80 KB/s，ESP-IDF 默认的 5.7 KB 不到 30 KB/s）。回复都很小。 |
| `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y`、`CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y`、`CONFIG_LWIP_TCPIP_TASK_PRIO=4` | 网络任务放在核 0，核 1 留给固件；lwIP 任务的优先级低于固件的界面和音频（ESP-IDF 默认的 18 会抢占它们；Wi-Fi 自己的任务仍是 23）。 |
| `CONFIG_LWIP_IPV6=n` | 只用 IPv4，和 203 Launcher 一样：代码更少，没有 IPv6 的表。 |
| `CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS=y` | 在线服务的 TLS 会话在两次请求之间复用：第一次之后，空中不再传证书链，CPU 也不再做密钥交换和证书校验。 |
| `CONFIG_SPIRAM_USE_MEMMAP=y` | PSRAM 只给明确申请的代码（examples/psram_heap）。 |
| `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` | Wi-Fi、lwIP 和 supplicant 的 `.bss`（约 14 KB）放到 PSRAM，只在联网时使用。它可以和 `SPIRAM_USE_MEMMAP` 一起用，但 ESP-IDF 把它放在 PSRAM 的开头：自己把 PSRAM 加进堆的固件必须从 `_ext_ram_bss_end` 之后开始加（两个示例都这样做），否则堆会把这些变量的内存再分出去。ESP-IDF 随之去掉 `SPIRAM_IGNORE_NOTFOUND`：PSRAM 起不来时板子不再启动。 |

不检查证书的有效期（`CONFIG_MBEDTLS_HAVE_TIME_DATE` 保持关闭：设备没有时钟）。

**限制。** 2.4 GHz 网络，有密码（WPA、WPA2、WPA3 个人版）或开放；不支持企业网络；隐藏网络只能作为上次连接的网络连上（扫描看不到它的名字）。不跟随重定向，不保持连接（每个请求一个连接），不支持代理和 HTTP/2。只信任 ESP-IDF 5.3 证书包里的根证书：Let's Encrypt 较新的根证书（YR、YE）不在里面，用它们的网站只要还发送由 ISRG Root X1 交叉签名的证书链（2026 年仍然如此）就能连上。回复正文只保留调用者缓冲区放得下的部分。

## 社区在线服务（任何固件运行时都能收到）

公开接口：[`net/presence.h`](net/presence.h)；它的判断逻辑写成纯函数、在电脑上测试：[`net/presence_logic.h`](net/presence_logic.h)。

在 203 社区网站上，固件页面可以把这个固件发到登录用户已绑定的设备上，只安装，或安装后启动；「我的设备」还可以升级设备的 Launcher。203 Launcher 在它的菜单里接收这些命令；在线服务让设备在**任何基于 HAL 的固件**运行时也能收到（社区也能看到它在线、正在运行固件），不论设备正在做什么。打开 `CONFIG_CARDENZA_HAL_PRESENCE`（menuconfig › CardenzaHAL；**默认打开**）时由 `CardenzaHAL::Init()` 启动，什么都不做的固件也会在线：

1. `Init()` 之后 `CONFIG_CARDENZA_HAL_PRESENCE_START_DELAY` 秒（10 秒），一个任务（优先级 1，核 0，8 KB 栈）读取 Launcher 的设置。只有 Launcher 把设备绑定到了社区（`l_account`，并且是社区自己的域名）、保存了 Wi-Fi 网络、并且 **设置 › 账号 › 接收固件** 是打开的，它才继续；否则任务结束（`State::Idle`），这次运行中不再占用任何资源。
2. 它通过 [Net](#网络wi-fi-与-https) 连上 Wi-Fi（固件可以共用这个连接：`Net::Connect` 直接加入，`Net::Disconnect` 只是自己放手，不会把连接从服务手里拿走），在固件允许的时候保持连接（见下），两次信标之间进入调制解调器睡眠。
3. 每 15 秒（以回复里的 `poll` 为准）用设备令牌请求一次 `GET <api>/launcher/inbox?from=firmware`，同时带上 `X-Firmware-Name`、`X-Firmware-Version`（应用描述里的项目名和版本）和 `X-Image-Digest`（镜像末尾的 SHA-256，`Net::RunningImageDigest`），让社区知道正在运行什么。这个请求就是设备的在线状态：网站只允许发给 40 秒内请求过的设备，否则显示“设备没开机或没联网”。
4. 收到的命令会**交给 Launcher**：先调用固件的钩子（`SetHandoverHook`：保存不能丢的东西，或者拒绝）；把命令（编号、动作 `install`、`install_start` 或 `update_launcher`、社区为私有固件发来的解锁令牌、社区地址）写进共享 NVS 的 `l_pending` 命名空间；告诉社区（`POST <api>/launcher/commands/<id>` `{"status":"handover"}`）；然后重启进入 Launcher（`System::RestartIntoLauncher`，见下）。Launcher 先清掉 `l_pending`，再向社区要这条命令（`GET <api>/launcher/commands/<id>`，其中的 Item 带着构建的签名声明），像处理自己收件箱里的命令一样执行并逐步汇报。格式由 Launcher 定义：203-launcher `docs/SHARED_SETTINGS.md`。同一个回复里的其他命令立即拒绝（`failed`，“Unsupported command” 或 “Device busy”），网站不用一直等。

| 调用 | 作用 |
| --- | --- |
| `Start()` | 启动服务，只启动一次（打开选项时由 `Init()` 调用）。 |
| `Enable(bool)` | 默认打开。关闭后不再请求，并放开 Wi-Fi（固件没有通过 Net 占用时 Wi-Fi 会关掉）；服务停下后才返回，所以要在可以等待的任务里调用。在 `Init()` 之前调用可以让服务根本不启动。固件要自己运行 Wi-Fi 之前先关掉它（Wi-Fi 只能有一个主人：服务开着时，固件自己的 `esp_wifi_init` 会失败）。 |
| `Planned()` | 这次运行中服务会不会上线（打开了、绑定到允许的社区、保存了网络、接收固件打开）。只读一次设置（只读，几毫秒）；固件可以在启动时问一下，给 Wi-Fi 留出空间。 |
| `SetActivity(Activity)` | 固件正在做什么；立即返回，任何任务都可以调用。`Idle`（默认）：Wi-Fi 可以连上，请求照常。`RealTime`（正在出声）：不启动 Wi-Fi（一次运行里第一次启动会校准射频，让核 0 忙一阵），已经连上的照常请求。`Quiet`：服务放开 Wi-Fi（内存还给固件，正在进行的请求取消），活动改变之前不再请求。 |
| `SetRamFloor(bytes)` | 固件给自己留的内部内存（默认 24 KB）：只有在留出这么多之外还放得下 Wi-Fi（第一次连上时实测，之前按 40 KB 算）才启动 Wi-Fi；空闲内部内存低于它时放开 Wi-Fi（之后 60 秒内不再连，固件一直需要时翻倍，最多 10 分钟）。 |
| `GetState()` | `Off`、`Idle`（这次运行中无事可做）、`Connecting`、`Online`、`Waiting`（失败后或正在让路）、`SignedOut`（社区拒绝了令牌）、`HandingOver`。 |
| `GetReport()` | 用于诊断：状态、在等什么（`Activity`、`AudioLate`、`Ram`、`FirmwareRequest`、`Failure`）、成功和失败的请求数、Wi-Fi 启动和放开的次数、Wi-Fi 占用的内部内存（`wifiBytes`）、上一次请求的耗时、它看到的空闲内部内存。 |
| `SetHandoverHook(hook, context)` | `bool hook(const Handover&, void*)` 在重启前、在服务的任务里运行：命令编号和动作、是否启动或升级 Launcher、固件名称和版本（用来显示提示）。返回 `false` 表示拒绝（网站显示 “Refused by the running firmware”）。 |

**它会让路**（`Logic::Decide`，按顺序）。`Quiet` 时放开 Wi-Fi。空闲内存低于固件的下限时，把 Wi-Fi 的内存还给固件。扬声器 DMA 跟不上时（`Audio::GetTxCounters()`：发出时没有新数据的块），什么也不开始。没连 Wi-Fi 时，`RealTime` 就等着；只有下限加上 Wi-Fi 的开销（和它任务栈的一整块）放得下时才启动 Wi-Fi。连着 Wi-Fi 时，固件自己的 Net 请求优先；只有下限加上一次请求的内存放得下时才发请求（TLS 在 PSRAM 时 12 KB、最大块 8 KB，否则 48 KB、20 KB）。某次请求期间音频断了流，之后 60 秒内不发请求，每次翻倍、最多 5 分钟；失败后等 5 秒，每次翻倍、最多 5 分钟。它**不会**因为扬声器在播放就停：一直在写音频的固件（合成器连静音也在渲染）那样就永远收不到命令了，这种固件应当说 `RealTime` 或 `Quiet`。任务优先级是 1、在核 0 上运行，只用别的任务不用的时间；按参考 sdkconfig，Wi-Fi 的任务（23）和 lwIP 的任务（4，低于固件的界面和音频）也在核 0，Wi-Fi 和 lwIP 从闪存运行，IRAM 留给固件。服务不写闪存（交接那一次除外），也没有长时间的临界区。

**有实时音频的固件怎么做。** 出声时说 `RealTime`（Wi-Fi 不会在声音中间启动，已经连着的保持连接），需要每个字节、每个周期的时段（录音、大的载入）说 `Quiet`，其余时候 `Idle`；内存下限设成运行时需要分配的量（CardenzaSynth 留 32 KB：它的效果器和引擎要检查 16-20 KB 空闲）。从不调用这些的固件是 `Idle`、下限 24 KB。

**令牌发到哪里。** 设备上的每个固件都能读到设备令牌（见 Net），所以把它当作半公开的：服务只通过 https 把它发给 `presence_logic.h` 里的域名（`community.203.io`、`cardenza.203.io`，端口 443），绑定里写的是别的地址时根本不发请求（SD 卡或测试设置都不能把它引到别处）。它只用来请求收件箱和汇报命令（社区的设备凭据是最小权限的）。`CONFIG_CARDENZA_HAL_PRESENCE_LOCAL_HTTP`（仅调试版本，默认关闭）额外允许用 http:// 连环回或私有 IPv4 地址上的测试社区。日志里从不出现令牌、解锁令牌或 URL 的路径。收到 401 `signin_required`（或 403 `banned`）时，先再读一次 `l_account`（也许 Launcher 换了令牌：它每天轮换一次，但从不在固件运行时轮换），仍然不行就停到下次启动；`l_account` 本身从不改动。每次连上 Wi-Fi 时读取绑定。

**回到 Launcher。** [`system/launcher.h`](system/launcher.h) 的 `System::RestartIntoLauncher()` 写入 Launcher 的重启票据（RTC 快速内存里的两个字，Launcher 的 bootloader 读取后清除：进入 Launcher 首页，即使打开了“Resume after reset”，也不显示“Boot last app”倒计时），把 USB PHY 交还给 USB-Serial-JTAG，然后重启。`USB::RestartIntoLauncher()`（0x40 命令）现在通知 USB 主机后也这样做。没有 Launcher 时就是重启固件。

**唯一一次写 NVS。** 交接时只往共享 NVS 分区写 `l_pending`（不写别的），在重启前写一次：固件自己打开了分区就用它，否则像 Launcher 启动时那样以读写方式打开 NVS（它会修复断电留下的半截写入，Launcher 片刻之后本来也会这样做）。只有擦除才能打开的分区不去动它，交接失败（“Could not hand over to the Launcher”）。写闪存会让缓存停几毫秒：重启前按键灯可能闪一下。

**开销**（ESP-IDF 5.3.5，2026-11-17，[`examples/launcher_net`](examples/launcher_net/sdkconfig.defaults) 的参考 sdkconfig）。

| | 实测 | 怎么测的 |
| --- | --- | --- |
| 静态内部内存 | **+22.6 KB**（IRAM 代码 +13.1 KB：Wi-Fi、PHY 和系统必须留在 IRAM 的代码；.data +8.1 KB；.bss +1.4 KB）；另有约 12 KB 的 Wi-Fi 和 lwIP .bss 放在 PSRAM | `examples/community_presence` 打开与关闭选项对比（`esp_idf_size --diff`）；CardenzaSynth：+21.3 KB |
| 闪存 | **+610 KB**（Wi-Fi、lwIP、supplicant、mbedTLS、证书包） | 同上 |
| Wi-Fi 连着时的内部内存 | 在板子上实测：`GetReport().wifiBytes`，以及日志里的 `Wi-Fi up: it took … of internal RAM`；预计 30-45 KB（4 个静态接收缓冲区，Wi-Fi、lwIP 和事件循环的任务，驱动，supplicant），另加本任务的 8 KB 栈 | Net 以前在板子上的实测：6 个静态缓冲区时 40-60 KB |
| 每次请求 | 复用 TLS 会话：约 2 KB 流量，lwIP 和 Wi-Fi 任务里几毫秒的 CPU；一次连接的第一个请求是完整握手（约 6 KB，P-256 的 ECDHE 是软件算的：优先级 1 上几十毫秒，另有片刻 30-50 KB 的 TLS 内存，在 PSRAM）。16 KB 的回复和其中的命令在 PSRAM；两次请求之间保留一个 2 KB（内部内存）的客户端 | 日志里第一次请求那一行，`GetReport().lastAskMs` |
| 两次请求之间 | 调制解调器睡眠：Wi-Fi 为接入点的信标醒来（每秒几次，每次核 0 上不到一毫秒） | |

成功的请求只在调试级别记录，固件放开 Wi-Fi 之后重新连上的例行步骤也是：只有发生了能解释问题的事情时（第一次上线、为什么等内存、因为内存放开 Wi-Fi、失败、令牌被拒、交接），服务才往固件的日志里加一行。

**关闭。** `CONFIG_CARDENZA_HAL_PRESENCE=n`：服务和 Net 都不会被链接，接口什么也不做（`GetState()` 是 `Off`，`Planned()` 是 false）。网页目标没有网络：接口存在，服务不存在。

## 硬件资源归属

GPIO 以 [`board/pins.h`](board/pins.h) 为准。应用应避免重新配置 HAL 占用的资源：

| 资源 | 使用模块 |
| --- | --- |
| I2C0，GPIO1/2 | System、音频编解码器与应用 I2C 设备 |
| I2S1，GPIO41/43/42 | 音频输出 |
| I2S0，GPIO43/46 | PDM 麦克风；GPIO43 与输出共用 |
| LEDC timer 0 / channel 7 | 屏幕背光，APB 时钟 |
| LEDC timer 1 / channel 6 | 按键灯全局亮度，APB 时钟 |
| GPIO47/48/21 | 按键灯使能、时钟、数据 |
| GPIO0 | Func 按键与 ROM 启动选择 |
| 内部 USB PHY | USB 设备或 USB-Serial-JTAG |
| Wi-Fi 射频 | Net：固件在 `Connect` 和 `Disconnect` 之间，以及在线服务运行期间（固件自己运行 Wi-Fi 之前先 `Presence::Enable(false)`） |

## 板子默认配置、分区表与刷机

HAL 附带了这块板子本身的事实，作为固件可以直接用的默认值（都不强制）：

- [`board/sdkconfig.defaults`](board/sdkconfig.defaults)：16 MB 闪存，四线 120 MHz（和 203 Launcher 的 bootloader 一样）；2 MB PSRAM，120 MHz；FreeRTOS 1000 Hz（`Display::Present` 每发一条带子都要等，100 Hz 时整屏要 170 ms）；FAT 用 512 字节扇区和长文件名。在 `SDKCONFIG_DEFAULTS` 里把它放在最前，自己的默认值放后面：
  ```cmake
  set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/components/CardenzaHAL/board/sdkconfig.defaults;sdkconfig.defaults")
  include($ENV{IDF_PATH}/tools/cmake/project.cmake)
  ```
- [`board/sdkconfig.debug`](board/sdkconfig.debug)：调试版本用。任务卡住时板子带着调用栈重启（任务看门狗触发 panic、检查空闲任务、统计各任务的 CPU 占用）。
- [`board/partitions.csv`](board/partitions.csv)：默认分区表。**设备上的分区表归 203 Launcher 管**：经 Launcher 安装的固件放在 Launcher 划出的分区里。这张表只在直接刷机时才用到，此时它把 Launcher 的区域原样留在 Launcher 的位置（NVS、otadata、Launcher 自己的分区留空、coredump），固件放在它们后面的 `ota_0`（0x170000，4 MB）。用 `CONFIG_PARTITION_TABLE_CUSTOM=y` 和 `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="<HAL 的路径>/board/partitions.csv"` 指定它。0x170000 以下不要放别的东西。
- [`tools/cardenza_flash.py`](tools/cardenza_flash.py)：安全地刷入一次构建。它先读板子上的分区表，**板子上装着 Launcher 就拒绝**（整片刷会抹掉 Launcher 和它的已装软件列表）；请改用 Launcher 安装；开发时也可以用 `--app-into <分区名>` 只把程序镜像写进 Launcher 的某个程序分区（Launcher 的开发流程：之后在 Launcher 里启动或登记它）。`--replace-launcher` 会有意抹掉 Launcher。固件自带的分区表会压到 Launcher 时，它也会提醒。
  ```sh
  python3 components/CardenzaHAL/tools/cardenza_flash.py -p /dev/ttyACM0 build
  ```

## 电脑端工具

给板子所连电脑用的命令行工具（Python 3）：

- [`tools/cardenza_usb.py`](tools/cardenza_usb.py)：`usb/usb.h` 的电脑端。`list` 列出板子的 HID、MIDI 和音频设备；`command identify|reboot|launcher|download` 重启它（进 Launcher，或进 ROM 下载模式：固件开了 USB 就没有串口，要这样才能刷）；`console <秒数>` 打印控制台和日志；`app <文字>` 和固件自己的应用通道对话。固件自己的工具可以导入它。需要 `pip install hidapi`；MIDI 走 macOS 的 CoreMIDI。
- [`tools/cardenza_flash.py`](tools/cardenza_flash.py)：刷机时不会抹掉 Launcher（见上）。
- [`tools/check_submodule.py`](tools/check_submodule.py)：固件以 git 子模块使用 HAL 时，HAL 有本地改动、或停在上游 `main` 没有的提交上就报错，并且禁止从里面推送。每次构建前运行它，这样 HAL 的改动都在上游做，所有固件用的是同一份 HAL。

## 版本与构建信息

[`cmake/firmware_info.cmake`](cmake/firmware_info.cmake) 给固件提供版本号和每次构建的身份，日志或崩溃都能对上是哪一次构建：

```cmake
include(${CMAKE_CURRENT_LIST_DIR}/components/CardenzaHAL/cmake/firmware_info.cmake)
cardenza_read_version(${CMAKE_CURRENT_LIST_DIR}/version.txt PROJECT_VER)   # 在 project() 之前：Launcher 显示的版本
# 在打印它的组件里：
cardenza_target_build_info(${COMPONENT_LIB} ${PROJECT_DIR} HEADER MyBuildInfo.h NAMESPACE my::build)
```

`version.txt` 写 `主.次.修订`。生成的头文件里有 `kVersion`、`kHash`（Git 提交）、`kDirty`（带本地改动构建，HAL 的改动也算）和 `kTimeUtc`。每次开机连同 ELF 的哈希（`esp_app_get_elf_sha256`）一起打印出来，并保存给过别人的每个版本的 ELF：解析调用栈要用它（`xtensa-esp32s3-elf-addr2line -pfiaC -e <固件>.elf <地址>`）。

## 开发须知、示例与发布

- [`docs/developing.zh-CN.md`](docs/developing.zh-CN.md)：芯片和工具链容易忽略的地方（两种内存、开 PSRAM 后的原子操作、热路径里的 `memcpy` 和除法、写闪存会停掉什么、读文件、USB 与串口、排查卡死），以及固件在 203 Launcher 下应该怎样运行。
- [`examples/psram_heap`](examples/psram_heap)：一个小固件，把 PSRAM 加进堆、只给明确申请的代码用，并替换 `new`，让申请失败变成空指针而不是重启。`idf.py -C examples/psram_heap build`。
- [`examples/launcher_net`](examples/launcher_net)：读出 Launcher 的设置，连上它的 Wi-Fi，发一个 HTTPS 请求再把一切关掉，每一步都打印耗时和内存；它的 `sdkconfig.defaults` 是使用 Net 的固件的参考配置。`idf.py -C examples/launcher_net build`。
- [`examples/community_presence`](examples/community_presence)：带在线服务的 `CardenzaHAL::Init()`、交接钩子、在日志里打印服务的状态。`idf.py -C examples/community_presence build`。
- [`examples/hal_check`](examples/hal_check)：通过 HAL 逐个检查板子：每个键的格子和按键灯、音频任务为每个按住的键发一个音、HOME。同一份源码既能编译到板子上，也能编译成网页版。
- [`.github/actions/launcher-publish`](.github/actions/launcher-publish/action.yml)：用 App 的专用令牌把固件发布到 203 Launcher hub 的 GitHub Action（推送是 Nightly，`vX.Y.Z` 标签是正式版）。
- **AI 代理与 203 社区**：要发布固件版本、读取用户需求、查看检查报告或把固件发到设备，先装一次 203 社区 skill：`curl -fsSL https://community.203.io/skill/install.sh | sh`（装到 `~/.claude/skills/203-community`；说明见 https://community.203.io/agents）。

## 网页模拟器

HAL 的网页目标（[`web/`](web)）把基于 CardenzaHAL 写的固件，用同一份源码编译成在浏览器里全速运行的 WebAssembly 模拟器：屏幕、4 × 14 个按键和按键灯、HOME、扬声器、SD 卡和 USB MIDI 都在。社区的试玩页放的就是它，没有板子时也可以先用它试改动。

### 编译模拟器版本

需要 [Emscripten](https://emscripten.org/docs/getting_started/downloads.html)（`emsdk install latest`、`emsdk activate latest`）和 CMake 3.20 以上；不需要 ESP-IDF。在固件工程目录（`idf.py` 用的顶层 `CMakeLists.txt` 所在的目录）运行：

```sh
python3 components/CardenzaHAL/tools/cardenza_web.py build .
python3 components/CardenzaHAL/tools/cardenza_web.py serve build-web/site   # http://localhost:8000/
```

`build` 用 HAL 自带的 ESP-IDF 构建适配层（`web/idf`）代替 ESP-IDF 来配置工程：你的各个 `CMakeLists.txt`、`main/`、`components/`、`EXTRA_COMPONENT_DIRS`、`managed_components/` 以及 `sdkconfig`（或 `SDKCONFIG_DEFAULTS`）都原样使用。输出目录（默认 `build-web/site`，可用 `--out` 指定）里有 `index.html`、`<工程名>.js`、`<工程名>.wasm`、`cardenza-sim.js` 和 `sim.json`（目标、固件名称和版本、HAL 的版本和提交、键位表、每个文件的大小和 SHA-256）。任何静态网页服务器都能托管。它不需要跨源隔离（不需要 COOP / COEP：没有用 SharedArrayBuffer），只是从另一个源显示时要发 `Access-Control-Allow-Origin: *`（在 `<iframe sandbox="allow-scripts">` 里总是如此，那里的源是不透明的），`.wasm` 最好以 `application/wasm` 发送（不是的话就从下载的字节编译）。在这样的框架里浏览器不给它存储（SD 卡只在内存里），也不给 MIDI 和麦克风；单独打开时，SD 卡和 NVS 保存在 IndexedDB 里。`serve` 会发这些响应头，并在第二个端口的 `/__sandbox.html` 以同样的框架显示网站。`--hal <目录>` 指定另一份 CardenzaHAL；默认用固件自带的那份（有 `web/` 时），否则用工具所在的那份（会给出警告）；`--debug` 带断言编译。

退出码：`0` 编译成功；`1` 你的代码编译不过（后面跟着编译器输出）；`2` 用法错误，或不是 ESP-IDF 工程；`3` 固件没有遵循 HAL（stderr 每行一项：`unsupported: <函数>`、`unsupported-header: <头文件>`，或汇编和预编译库的 `unsupported-source: <文件>`）；`4` 缺少 Emscripten 或 CMake（附安装方法）。

页面铺满窗口（板子 7:4，黑底），电脑键盘按按键位置对应（键位表在 `sim.json` 里，试玩页会显示），也可以用触摸或鼠标按页面上的键（右键锁住一个键）；第一次点击或按键后开始出声；`app_main` 运行后向外层页面发送 `{"cardenza_sim":"ready"}`，接着是 `{"cardenza_sim":"keys","keys":[…]}`；另外还有 `restarted`、`crashed`、`stopped`（10 秒内崩溃三次）、`ir` 和 `app_report`。外层页面可以用 `{"cardenza_sim":"app","data":[…]}` 向固件的应用通道发送数据。URL 参数：`?sd=0`（没有卡）、`?sdmb=<MB>`（卡的大小，默认 8192）、`?log=1`（在板子下面显示控制台）。`esp_restart` 和崩溃会在同一页面里启动一个新实例（声音、屏幕和 MIDI 保持）。

固件的 CMake 可以用变量 `CARDENZA_HAL_WEB`（代码里用同名的宏定义）判断是否在编译网页版，只在少数必须的地方用：比如自带网页版、靠 `__EMSCRIPTEN__` 切换的第三方库（`__EMSCRIPTEN__` 仍然定义着：C 库需要它）。

### 什么叫"遵循 HAL"

固件只通过 HAL 的接口和它给出的原生句柄（屏幕的 panel 和 panel IO、I2S 通道、I2C 总线、SD 卡上的文件），以及 FreeRTOS 和 ESP-IDF 的系统服务访问硬件，就是遵循 HAL。网页目标提供的正是这些，行为与设备一致：

| 范围 | 网页目标上 |
| --- | --- |
| FreeRTOS | 任务、优先级、两个核（`xTaskCreatePinnedToCore`）、按 tick 的延时、队列、信号量、带优先级继承的互斥量、队列集、任务通知、事件组、软件定时器、流缓冲和消息缓冲、临界区。pthread、`std::thread`、`std::mutex`、`std::condition_variable` 和 sleep 都以任务运行。 |
| 时间 | `esp_timer`（任务和 ISR 两种派发）、`esp_timer_get_time`、`xTaskGetTickCount` 跟随真实时间。 |
| 内存 | 内部 RAM（`app_main` 开始时约 300 KB 可用）和 2 MB PSRAM 是两个独立的堆，容量与设备相同；`malloc`、`new`、任务栈和 DMA 缓冲都像在板子上一样从中分配；`heap_caps_*`、`CONFIG_SPIRAM_USE_MALLOC / CAPS_ALLOC / MEMMAP`、`esp_ptr_external_ram` 都可用。在板子上内部 RAM 不够的固件，在这里也会在差不多的地方不够。 |
| 屏幕 | `Display::*` 就是 HAL 自己的代码；panel 和 panel IO 句柄驱动一个 ST7789 模型（窗口、MADCTL、反色、睡眠、字节序），带 SPI 总线的耗时（整屏 17 ms）和 `on_color_trans_done`。GPIO38 上的 LEDC 就是背光。 |
| 按键、按键灯、HOME | `Keyboard::*` 把页面的按键交给 HAL 自己的去抖和四键上限；按键灯显示扫描会输出的亮度和总亮度。`System::GetFuncButton()` 和 GPIO0 就是 HOME。 |
| 音频 | `Audio::*`，以及经 I2C 访问的 ES8156 编解码器寄存器（启动耗时、音量）；TX 通道的 DMA（描述符 × 帧数）由页面的音频时钟按 44.1 kHz 取走，所以 `i2s_channel_write` 会像在板子上一样阻塞；有 `on_sent` / `on_send_q_ovf`。麦克风（`Mic::*`，PDM RX）在页面被允许时使用电脑的麦克风；GPIO43 归最后配置它的一方。 |
| 存储 | `Storage::*` 挂载 `/sdcard`（页面说没有卡时 `?sd=0` 就没有）；C 和 C++ 的文件操作都可用；`esp_vfs_fat_info`；NVS 随页面保存。 |
| USB | `USB::*`：MIDI 与电脑的 MIDI 端口互通（Web MIDI，在被允许时），系统 SysEx 命令，应用通道（也通向外层页面）；USB 音频和 HID 没有主机。 |
| 其他 | GPIO、GPTimer、`IR::Send` 用的 RMT（会解码显示）、ESP-IDF 日志、`esp_restart`（模拟器重新启动固件）、`esp_reset_reason`、`esp_random`、`esp_app_get_description`、任务看门狗的调用。 |

其他的都不提供，用到时编译失败并说明原因：其他 ESP-IDF 驱动和组件（Wi-Fi、蓝牙、UART、ADC、SPI 设备……）、寄存器访问（`soc/*_reg.h`）、屏幕或编解码器自己的初始化（归 HAL 负责）、Xtensa 和 ROM 内部、`esp_private/*`。所以能编译成网页版的固件就是遵循 HAL 的；编译不过的，会得到它越过 HAL 的那些调用的清单。

仍然存在的差别：浏览器的一个线程运行所有任务，在每次调用 FreeRTOS、ESP-IDF 或 HAL 时（以及两个核的任务之间）切换，所以一直循环、从不调用它们的任务会让其他一切停下（半秒后日志会点名）；`thread_local` 被所有任务共用；文件名区分大小写，打开着的文件也能改名；浏览器的时钟比板子粗（0.1 到 1 ms）；声音在 DMA 取走后约 60 ms 才到扬声器。

HAL 对网页目标的测试：`tests/web`（内核和各个模型，在电脑上运行）、`tests/web/browser`（无头 Chrome 里的页面）、`tests/web_tool_test.py`（构建工具）。

## 主机测试

[`tests/`](tests) 在电脑上检查 HAL，不需要板子和 ESP-IDF：纯计算部分（按键去抖和按键数上限、LED 帧、背光曲线，以及 Net 的：Launcher 保存网络的格式（用 Launcher 自己的代码生成的数据检验）、请求检查、尝试网络的顺序；在线服务的判断逻辑和 Launcher 的重启票据）、驱动源码在故意出错的假 ESP-IDF 驱动下运行（让每条出错路径都走一遍），以及网页目标的内核和各个模型（`tests/web`）。CI 在每次推送和每个 PR 上运行它们。

```sh
cmake -S tests -B build-tests
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

## 常见问题

| 现象 | 排查方向 |
| --- | --- |
| 找不到组件 | `REQUIRES` 对应 HAL 目录名；外部路径要在 `project.cmake` 前加入 `EXTRA_COMPONENT_DIRS`。 |
| TinyUSB 配置或编译冲突 | 由 HAL 提供 `usb/config/tusb_config.h`，移除冲突的 `esp_tinyusb` 配置。 |
| `Init()` 返回 `false` | 看 `LastInitReport()` 和模块日志；SD/IR 失败不会单独导致整体失败。 |
| 屏幕不亮 | 用 `Present()` 成功提交第一帧，并确认亮度非零。 |
| 按键灯不亮 | 确认已启用 LED，单键和全局亮度都非零。 |
| 换键事件丢失 | 比较完整快照；`isChanged()` 只检测数量变化。 |
| 没有声音 | 检查编解码器就绪、音量、声道模式、TX 启用和 PCM 连续写入；用过麦克风后需恢复输出引脚。 |
| SD 卡不可用 | 检查文件系统/卡和 `Storage::Init()`；HAL 不会在挂载失败时格式化。 |
| 开启 USB 后串口消失 | USB PHY 已切换。通过系统指令进入 Launcher/ROM 下载模式。 |
| USB 枚举成功但没有音频 | 在主机开启音频流，并在应用中供给/读取 USB 音频；HAL 不自动桥接硬件。 |

## Launcher 应用

上传和登记工具及完整使用步骤见 [tools/uploader/README.zh-CN.md](tools/uploader/README.zh-CN.md)。

## 致谢

- **[M5Stack Cardputer](https://docs.m5stack.com/en/core/Cardputer)**：一个很棒的项目。它是鸣片（Cardenza）的灵感来源，也是我们动手做鸣片的初衷，谢谢 M5Stack 团队。鸣片和这个 HAL 不兼容 M5Stack 的设备。
- **[ESP-IDF](https://github.com/espressif/esp-idf)**（Espressif）与 **[TinyUSB](https://github.com/hathach/tinyusb)**：这里每个模块所基于的框架，以及 `usb/usb.h` 背后的 USB 协议栈。
- **[Emscripten](https://emscripten.org)**：网页目标靠它把同一份固件源码跑在浏览器里。
