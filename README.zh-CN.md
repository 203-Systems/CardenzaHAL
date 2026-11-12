# Cardenza HAL

[English](README.md) | [简体中文](README.zh-CN.md)

CardenzaHAL 是 Cardenza（ESP32-S3）的 ESP-IDF 硬件层。你可以用它读取 56 键键盘、控制每颗按键灯、向 240 × 135 屏幕绘图、播放 PCM 音频、访问 SD 卡，以及给固件加入 USB MIDI、音频和 HID 功能。

HAL 负责硬件初始化，并提供原生 ESP-IDF 句柄；应用负责界面绘制、键位映射、音频处理、文件格式和主循环。本仓库是组件，需要接入固件工程使用。

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

`LastInitReport()` 提供上述六个模块的结果和 `overall`。整体初始化成功后会缓存结果，再调用一次不会重新挂载缺失的 SD 卡；需要单独调用 `Storage::Init()` 重试。麦克风和 USB **不在整体初始化范围内**；`cardenzaHAL.h` 也不包含 `usb/usb.h`。

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
- [`.github/actions/launcher-publish`](.github/actions/launcher-publish/action.yml)：用 App 的专用令牌把固件发布到 203 Launcher hub 的 GitHub Action（推送是 Nightly，`vX.Y.Z` 标签是正式版）。

## 主机测试

[`tests/`](tests) 在电脑上检查 HAL，不需要板子和 ESP-IDF：纯计算部分（按键去抖和按键数上限、LED 帧、背光曲线），以及驱动源码在故意出错的假 ESP-IDF 驱动下运行，让每条出错路径都走一遍。CI 在每次推送和每个 PR 上运行它们。

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
