# Cardenza HAL

[English](README.md) | [简体中文](README.zh-CN.md)

[官网](https://203.io/products/cardenza-mini-synthsiser) · [教程](https://cardenza.203.io/zh-Hans/) · [固件社区](https://cardenza.203.io/community) · [固件开发说明](https://cardenza.203.io/community/develop) · [203-CLI](https://github.com/203-Systems/203-CLI) · [示例固件：鸣片 CardenzaSynth](https://github.com/203-Systems/CardenzaSynth)

**要给鸣片做固件？先装 203cli，它是必须的。** `npm install -g 203cli && 203cli skill install`（Node.js 18 或更新；macOS、Linux、Windows 都一样）。下面每一步都用它，没装它，基于这个 HAL 的固件编译不了。只克隆 203-CLI 仓库不算安装。

CardenzaHAL 是 Cardenza（ESP32-S3）的 ESP-IDF 硬件层。你可以用它读取 56 键键盘、控制每颗按键灯、向 240 × 135 屏幕绘图、播放 PCM 音频、访问 SD 卡，给固件加入 USB MIDI、音频和 HID 功能，以及通过 203 Launcher 设置好的 Wi-Fi 上网。

HAL 负责硬件初始化，并提供原生 ESP-IDF 句柄；应用负责界面绘制、键位映射、音频处理、文件格式和主循环。本仓库是组件，需要接入固件工程使用；它的网页目标还能把你的固件编译成浏览器里的模拟器（见[网页模拟器](#网页模拟器)）。

## 在 CardenzaHAL 上做固件

有两样是必须的：**这个 HAL**（每个固件都只通过它访问硬件）和 **[203-CLI](https://github.com/203-Systems/203-CLI)**，即 203 社区的命令行 `203cli`：它建项目、把编译好的固件装到你的鸣片上、读日志，也是发布的唯一途径（手动或 CI）。它附带给 AI 编程助手用的 Agent Skill 和每台 203 设备的开发指引（鸣片的在 [`products/cardenza`](https://github.com/203-Systems/203-CLI/blob/main/products/cardenza/README.md)）。每一步的规则（人和 AI 读同一份）：[`docs/FIRMWARE-PROJECT.md`](docs/FIRMWARE-PROJECT.md)（英文，唯一的规范；[中文摘要](docs/FIRMWARE-PROJECT.zh-CN.md)）。

```sh
npm install -g 203cli && 203cli skill install                       # 203cli 和它的 Agent Skill（Node.js 18+，各系统都一样）
203cli new "My firmware" --summary "它做什么"                         # 新项目：这个 HAL 作为子模块，加上模板
cd my-firmware
python3 components/CardenzaHAL/tools/cardenza.py doctor   # 这台电脑还缺什么（ESP-IDF 5.3.5、Emscripten、203cli）
python3 components/CardenzaHAL/tools/cardenza.py build    # 首选的编译方式：先检查，再 idf.py build
203cli flash --bin build/my_firmware.bin                  # 通过 203 Launcher 用 USB 线装到你的鸣片上
203cli log                                                # 看它打印的日志
203cli login                                              # 每台电脑一次：浏览器里批准
203cli publish --dry-run && 203cli publish                # 第一个版本；之后由 CI 发布（见「发布」）
203cli sync                                               # 之后：.203/community.md，大家提了什么需求、说了什么
```

[`tools/cardenza.py`](tools/cardenza.py) 是首选的编译方式：编译前检查 ESP-IDF 5.3、这个 HAL 是干净的子模块、`203cli` 已安装（并已登录），shell 没激活 ESP-IDF 时它自己导出环境再编译。其他编译方式也检查 `203cli`：`idf.py build` 和网页目标没装它就停下，并说怎么装（[`cmake/require_203cli.cmake`](cmake/require_203cli.cmake)；CI 里不检查，发布那一步会自己装 203cli），并把 203cli 的更新提示作为警告打出来：有新版 `203cli`、这个 HAL 落后于它的 `main` 或不是干净的副本（`203cli updates`）。**不要对鸣片 `idf.py flash`**：那会抹掉 203 Launcher 和它装过的所有固件；`203cli flash` 通过 Launcher 安装。

## 硬件一览

鸣片有什么，在 HAL 的哪里。引脚：[`board/pins.h`](board/pins.h)；原理图和 PCB 模型：[Cardenza-Hardware](https://github.com/203-Systems/Cardenza-Hardware)。

| 部件 | 是什么 | 在 HAL 里 |
| --- | --- | --- |
| 芯片 | ESP32-S3，双核 240 MHz；512 KB 内部 RAM，2 MB 四线 PSRAM | ESP-IDF 本身；[板子默认配置](#板子默认配置分区表与刷机)、[`docs/developing.md`](docs/developing.md)（两种 RAM） |
| Flash | 16 MB，四线 120 MHz；0x170000 以下归 203 Launcher，其余由它划分，到它的 bootloader 声明的大小为止（16 MB；用 0.3.3 及以前的 Launcher 刷的板子是 8 MB，直到用 USB 重新写它的 bootloader）；一个应用最多 0x690000 字节，每台 Cardenza 都装得下 | [`board/partitions.csv`](board/partitions.csv)、[`board/sdkconfig.defaults`](board/sdkconfig.defaults) |
| 按键 | 56 键，4 行 × 14 列（编号 1..56）；同时最多可靠读出 4 个；键帽印着字符和 shift 后的字符，方向键在 `;` `.` `,` `/` 上 | [`keyboard/keyboard.h`](keyboard/keyboard.h)：`updateKeyList`、`keyList`、`isKeyPressing`；键帽：[`keyboard/legends.h`](keyboard/legends.h)；[键盘与按键灯](#键盘与按键灯) |
| 按键灯 | 每个键下一颗单色 LED，16 级，外加总亮度 | [`keyboard/keyboard.h`](keyboard/keyboard.h)：`setLED`、`setLEDFrame`、`setLEDGlobalBrightness`、`enableLED`；[`keyboard/led_light.h`](keyboard/led_light.h) |
| 屏幕 | 240 × 135 彩屏（RGB565），80 MHz SPI 接口的 ST7789，背光走 LEDC；整屏一帧约 8 ms | [`display/display.h`](display/display.h)：`Present`、`PresentChanged`、`SetBrightness`、`GetPanel`；[`display/presenter.h`](display/presenter.h)：`FramePacer`、`FrameStats`、`DoubleBuffer`；[屏幕显示](#屏幕显示) |
| 喇叭 | ES8156 解码器，I2S，44.1 kHz 16 位，单声道喇叭（可用立体声声道） | [`audio/audio.h`](audio/audio.h)：`Init`、`GetTxChannel`、`SetVolume`；[音频输出](#音频输出) |
| 麦克风 | PDM，44.1 kHz 单声道；时钟和喇叭共用 GPIO43 | [`mic/mic.h`](mic/mic.h)；[麦克风与音频共用引脚](#麦克风与音频共用引脚) |
| SD 卡 | microSD，4 线 SDMMC，FAT 挂在 `/sdcard` | [`storage/storage.h`](storage/storage.h)：`Init`、`IsMounted`、`CheckAfterIoError`；[`storage/app_dir.h`](storage/app_dir.h)：本固件的文件夹 `/sdcard/Apps/<AppName>`（[目录结构](docs/SD-CARD.zh-CN.md)）；[SD 卡存储](#sd-卡存储) |
| HOME | 侧边键（GPIO0；按一下打开[系统菜单](#系统菜单任何固件里按-home)；插 USB 或复位时按住进 ROM 下载模式） | [`system/system.h`](system/system.h)：`GetFuncButton`；[Func、I2C 与红外](#funci2c-与红外) |
| I2C | 共用总线（GPIO1/2），解码器在上面 | [`system/system.h`](system/system.h)：`GetI2CBus` |
| 红外 | 发射管（GPIO44，NEC 38 kHz） | [`ir/ir.h`](ir/ir.h)：`Send` |
| USB | 芯片自带的 USB PHY：HAL 的 USB 设备，由 `CardenzaHAL::Init` 启动（MIDI、双向音频、HID、WebUSB，需要时还有串口、把 SD 卡当 U 盘、把屏幕当无损画面流；每个能传指令的类都带系统通道：日志、信息、重启进 Launcher 或 ROM 下载模式），或 USB-Serial-JTAG（Launcher、ROM、esptool） | [`usb/usb.h`](usb/usb.h)；[USB 设备用法](#usb-设备用法)；电脑端：`203cli log`、`203cli board`、[`tools/cardenza_usb.py`](tools/cardenza_usb.py) |
| Wi-Fi、蓝牙 | 芯片自带（2.4 GHz Wi-Fi、蓝牙 5 LE） | 用 ESP-IDF 自己的驱动，不在 HAL 里（浏览器模拟器里没有：[网页模拟器](#网页模拟器)） |

## 发布

基于 HAL 的每个固件，203 Systems 自己的（Cardenza Synth、203 Launcher）和第三方的，都用同一种方式发布：发到 203 社区，203 Launcher 从社区的渠道安装和更新固件。

- 渠道跟着 Git 引用走，由 CI 用 [`.github/actions/community-publish`](.github/actions/community-publish/action.yml) 发布：推送到 `main` 是 **dev**（`<版本>-dev.<运行号>`），等于 `version.txt` 的 `vX.Y.Z` 标签是 **stable**（更新说明取 `CHANGELOG.md` 的 `## X.Y.Z`），`vX.Y.Z-beta.N` 标签是 **beta**。
- 受信任发布，不存任何密钥：作业（`permissions: id-token: write`）用 GitHub 的 OIDC 令牌登录，社区只接受固件主人在固件页「自动发布」里（或用 `203cli firmware publishers add`）关联过的仓库。
- 第一个版本手动发布（`203cli publish`），固件由此得到代码；工作流在 [`examples/template`](examples/template) 里。细节见 [`docs/FIRMWARE-PROJECT.md`](docs/FIRMWARE-PROJECT.md#8-publish)。
- 自己从源码编译、给自己的板子烧录仍然可以，但那不是发布。

## 使用前提

- 当前版本的 Cardenza 硬件，引脚定义见 [`board/pins.h`](board/pins.h)。
- **ESP-IDF 5.3 或更新版本**，已安装 ESP32-S3 工具链并激活 IDF 环境。这是依赖清单声明的最低版本，不代表所有后续版本都已通过兼容测试。
- C++17 或更新标准，使用 `.cpp` 文件引用 HAL 头文件。
- 启用 ESP-IDF Component Manager。[`idf_component.yml`](idf_component.yml) 会引入 `espressif/tinyusb` `~0.21.0`。

驱动依赖 ESP-IDF。备用的 CMake 目标 `CardenzaHAL` 同样需要嵌入式 ESP-IDF 环境，不能用来直接编译桌面程序或模拟器。

## 快速上手

下面的完整示例会点亮屏幕，并让按键灯跟随按下状态变化。`CardenzaHAL::Init()` 还会用默认的类启动 USB 设备（电脑上看到的是 "Cardenza"，203 社区和 `203cli` 都能识别并烧录它）；要自己选类，紧接着调用 `CardenzaHAL::USB::Init(config)`。

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

    // The screen's and key lights' brightness and the volume are the board's settings (system/settings.h):
    // Init applied them; a firmware does not set them itself.
    if (!Display::Present(framebuffer)) {
        ESP_LOGE("app", "Display submission failed");
        return;
    }

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

在固件根目录执行（工具会找到并导出 ESP-IDF 5.3）：

```sh
python3 components/CardenzaHAL/tools/cardenza.py build   # 或者在激活了 ESP-IDF 的 shell 里：idf.py build
203cli flash --bin build/cardenza_hello.bin              # 通过 203 Launcher 安装（不要 idf.py flash：会抹掉 Launcher）
203cli log                                               # 通过 USB 看日志
```

没有 203 Launcher 的板子（裸芯片）才用 `idf.py -p <PORT> flash monitor`。运行后，黑色屏幕顶部出现一条白色横条；按住哪个键，哪个键就亮，松开后熄灭。没有 SD 卡也可以运行。示例初始化了音频硬件，但不会播放声音。

这些命令用于你的固件工程。通过 Launcher 分发的应用还需要匹配的分区布局和元数据，见 [Launcher 应用](#launcher-应用)。

## 初始化与错误处理

[`CardenzaHAL::Init()`](cardenzaHAL.cpp) 按下表顺序初始化：

| 模块 | 是否影响 `Init()` 成功 | 行为 |
| --- | --- | --- |
| System | 是 | 初始化共享 I2C 总线和 HOME 键；失败则立即返回 |
| Keyboard | 是 | 启动后台按键与 LED 扫描 |
| Audio | 是 | 配置单声道 TX 并启动 ES8156；新建的 TX 通道保持禁用 |
| Display | 是 | 准备屏幕，提交一帧后才显示 |
| Storage | 否 | 尝试挂载 `/sdcard` |
| IR | 否 | 准备红外发射器 |

`LastInitReport()` 提供上述六个模块的结果和 `overall`。整体初始化成功后会缓存结果，再调用一次不会重新挂载缺失的 SD 卡；需要单独调用 `Storage::Init()` 重试。麦克风和网络（[Net](#网络wi-fi-与-https)）**不在整体初始化范围内**；`cardenzaHAL.h` 也不包含 `usb/usb.h` 和 `net/net.h`。在各模块之前，`Init()` 先用默认的类启动 [USB 设备](#usb-设备用法)（`CONFIG_CARDENZA_HAL_USB_AUTOSTART`，默认打开；一秒后才连接，除非你的 `USB::Init(config)` 先到），不管别的模块是否失败，板子都能被找到、被烧录；一个固件如果连续三次在启动后 15 秒内崩溃，第四次就不再运行它：`Init()` 直接重启进 203 Launcher 的首页（`CONFIG_CARDENZA_HAL_CRASH_LOOP_LAUNCHER`，默认打开；运行得更久或断电都会清零），坏掉的固件永远不需要按 BOOT 键。打开 `CONFIG_CARDENZA_HAL_PRESENCE`（默认打开）时，`Init()` 还会启动一次[社区在线服务](#社区在线服务任何固件运行时都能收到)，不论各模块的结果如何。

只需要部分功能时，可以分别初始化各模块。使用共享 I2C 或 HOME 键前，先调用 `System::Init()`，并检查各模块的返回值。音频编解码器初始化失败后可以再次调用 `Audio::Init()`，重试频率限制约为每秒一次。

## 键盘与按键灯

接口：[`keyboard/keyboard.h`](keyboard/keyboard.h)、[`keyboard/types.h`](keyboard/types.h)；键帽上印的是什么：[`keyboard/legends.h`](keyboard/legends.h)。

鸣片的键帽，从上到下一行一行（`y`），带按键编号。每个键自己的字符或名字印成黑色，按住 shift 时打出的字符印成黄色（下面写在第二个），另有四个键印着黄色箭头（up 上、down 下、left 左、right 右）：

```text
y   keys   x=0   1      2    3    4    5    6    7    8    9    10        11        12         13
0   1..14  esc   1 !    2 @  3 #  4 $  5 %  6 ^  7 &  8 *  9 (  0 )       - _       = +        del
1  15..28  tab   q      w    e    r    t    y    u    i    o    p         [ {       ] }        \ |
2  29..42  fn    shift  a    s    d    f    g    h    j    k    l         ; : up    ' "        ok
3  43..56  ctrl  opt    alt  z    x    c    v    b    n    m    , < left  . > down  / ? right  space
```

del 印的是 ←（退格），shift 印 ⇧，space 印 ␣；ok 就是回车。左边一列从上到下是 esc、tab、fn、ctrl：fn 是第三个键（29），左下角的键是 ctrl（43）。屏幕左侧的侧键 HOME 不在矩阵里（[HOME 键](#home-键i2c-与红外)）。

用键帽上印的，不要自己另编一套：

- **方向键是 `;` 上、`.` 下、`,` 左、`/` 右**，就是那四个黄色三角；不是 HJKL、IJKL 或 WASD。
- **文字输入**：每个键打出它的字符，按住 shift 打出黄色的那个；这时方向键是 fn + `;` `.` `,` `/`。del 删除光标前的字符，ok 结束输入。
- 不打字的画面（列表、菜单、游戏）可以直接用 `;` `.` `,` `/` 当方向键，203 Launcher 和系统菜单就是这样。
- **ok 确认，esc（或 del）返回。** HOME 归系统菜单。

[`keyboard/legends.h`](keyboard/legends.h)（纯头文件，模拟器和测试也能用）按编号给出这些：`Legend(key)` 或 `Legend({x, y})` 给出这个键的 `name`（`"esc"`、`"q"`、`";"`、`"ok"`、`"space"`）、`character` 和 `shifted`（可打印 ASCII；不打字的键是 `0`：esc、del、tab、fn、shift、ok、ctrl、opt、alt）以及 `arrow`；`Typed(key, shift)` 是它打出的字符；`KeyNumber({x, y})`、`KeyPosition(key)` 互相换算；`kKeyUp`、`kKeyDown`、`kKeyLeft`、`kKeyRight`、`kKeyEsc`、`kKeyOk`、`kKeyDel`、`kKeyTab`、`kKeyFn`、`kKeyShift`、`kKeyCtrl`、`kKeyOpt`、`kKeyAlt`、`kKeySpace` 是各个键的编号：

```cpp
#include "keyboard/legends.h"
namespace Keyboard = CardenzaHAL::Keyboard;

// 刚按下的键（keyList() 里上一帧还没有的 Point2D_t），在列表画面里：
switch (Keyboard::KeyNumber(key)) {
    case Keyboard::kKeyUp: MoveUp(); break;      // ;
    case Keyboard::kKeyDown: MoveDown(); break;  // .
    case Keyboard::kKeyOk: Open(); break;
    case Keyboard::kKeyEsc: Back(); break;
}
// 在输入框里：esc、del、tab、ok 和修饰键是 0。
const char typed = Keyboard::Typed(key, Keyboard::isKeyPressing(Keyboard::kKeyShift));
```

- 坐标范围为 `x = 0..13`、`y = 0..3`。按键编号为 **1..56**，公式是 `y * 14 + x + 1`。
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

接口：[`display/display.h`](display/display.h) 和 [`display/presenter.h`](display/presenter.h)。指南（撕裂和数字）：[docs/DISPLAY.zh-CN.md](docs/DISPLAY.zh-CN.md)。

- 横屏 **240 × 135**，RGB565；帧缓冲包含 `WIDTH * HEIGHT` 个 16 位像素，共 **64,800 字节**，行跨度为 `WIDTH`。面板配置为小端 RGB 数据。
- `Present(framebuffer)` 同步提交全屏。帧缓冲可以在 PSRAM 中，HAL 使用内部 DMA 小块转发。调用返回后即可复用调用方的帧缓冲，失败时也一样。
- 局部刷新时，先修改完整帧缓冲，再提交它里面的矩形区域：

```cpp
const CardenzaHAL::Display::Region dirty{0, 0, 40, 16};
bool ok = CardenzaHAL::Display::Present(framebuffer, &dirty, 1);
```

矩形宽高必须大于零，且完全位于屏幕内。首次提交及提交失败后的恢复会强制刷全屏；首次全屏完成后，传零个矩形表示没有像素变化。

`SetBrightness(0..255)` 接收感知亮度，HAL 已应用 gamma 2.2，无需重复校正。首次 `Present()` 成功前只记录亮度请求。冷启动会重置面板，软件重启会保留旧画面直到提交新帧。

使用原生渲染接口时，获取 `GetPanel()` / `GetPanelIO()` 句柄，完成绘制提交后调用无参数的 `Present()`，等待排队绘制完成并显示画面。不要把自定义 panel-IO 回调与帧缓冲版 `Present()` 混用。帧的提交放在同一个任务里（用 `DoubleBuffer` 时就是它的任务）。

**最快的用法**：每帧把整幅画面画进帧缓冲，在 **`FramePacer`** 循环里交给 **`PresentChanged(framebuffer)`**（每 10 ms 一拍，每两拍一帧：50 fps）：

```cpp
CardenzaHAL::Display::FramePacer pacer;
for (;;) {
    Update();                                         // 每拍都做
    if (pacer.DrawTick()) {
        Draw(frame);
        CardenzaHAL::Display::PresentChanged(frame);  // 只发变了的
    }
    pacer.Wait();
}
```

**`PresentChanged(framebuffer)`** 自己算出哪里变了，只发那些：每行按 30 像素一段算哈希，每条 8 行的带子找出变了的行、横向覆盖变了的段；每帧另外整条重发一带，约 0.34 秒整屏刷新一遍。帧缓冲要 4 字节对齐（`alignas(4)`，或用 `AllocateFramebuffer()`）；4.3 KB 的哈希表第一次用时才申请（有 PSRAM 就放 PSRAM），申请不到就整帧发送。它和其他 `Present` 二选一，不要混用。计算部分是 [`display/changed_regions.h`](display/changed_regions.h)，纯头文件，主机测试或自己负责发送的固件也能用。

所有提交帧缓冲的 `Present*` 都这样发：按窗口从左往右、顺着屏幕的扫描发（[`display/scan_order.h`](display/scan_order.h)：这块屏没有撕裂效应信号，这样画面移动时的斜向撕裂变成一条直线，而且更少），经过两块内部 RAM 的 DMA 块（前一个窗口在发时复制下一个），时钟 80 MHz（`CONFIG_CARDENZA_HAL_DISPLAY_SPI_MHZ`），一块发完等待的任务立刻醒来。整屏约 8 ms；画面不动时只发每帧重发的那条带子，约 0.5 ms。[`display/presenter.h`](display/presenter.h) 提供其余的：`PresentOptions`（哈希段宽、每帧重发几条带子、第二块 DMA 块）、`RequestWholeFrame()`、每帧花了多少（`LastFrame()`、`Totals()`、`SetFrameCallback`）、`FramePacer`、`AllocateFramebuffer`（PSRAM 或内部 RAM）和 `DoubleBuffer`（上一帧在发时画下一帧）。系统菜单打开时帧被挡住（`FrameStats::held`），关掉后第一帧整屏发。指南（撕裂、数字）：[docs/DISPLAY.zh-CN.md](docs/DISPLAY.zh-CN.md)。

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
5. 用 `i2s_channel_enable()` 启用 RX，然后使用 `i2s_channel_read()` 读取，读到的样本交给 `Mic::ApplyGain(samples, count, gainDb)`：增益是固件自己的，和固件自己的设置存在一起（不是板子的设置；-12 至 +36 dB，`mic/gain.h`；芯片的 PDM 接收没有自己的增益），超出范围时削顶。

恢复播放时，先停止读取并禁用 RX，再依次调用 `Audio::RestoreOutputPins()`、所需声道模式的 `Audio::Init()`，最后启用 TX；每步都检查结果。`InputChannel()` 可能初始化 RX，但不会启用它。HAL 不提供录音启停/读取封装，也不会自动仲裁引脚。

## SD 卡存储

接口：[`storage/storage.h`](storage/storage.h)（卡）和 [`storage/app_dir.h`](storage/app_dir.h)（本固件在卡上的文件夹）。每个固件都遵守的目录结构：[docs/SD-CARD.zh-CN.md](docs/SD-CARD.zh-CN.md)。

`Storage::Init()` 通过 4-bit SDMMC 把 FAT 文件系统挂到 **`/sdcard`**，最多同时打开五个文件（`CardenzaHAL::Init()` 会尝试挂载）；挂载失败时**不会格式化**。

**文件放在固件自己的文件夹 `/sdcard/Apps/<AppName>/` 里**，不放在卡的根目录，也不放进 `/sdcard/Launcher/`（203 Launcher 的）。`Storage::AppName()` 是 CMake 工程名改成安全的 FAT 文件夹名（或者 `CONFIG_CARDENZA_HAL_APP_NAME`，menuconfig：CardenzaHAL）；`AppDir()` 是这个文件夹，`AppPath("Settings/app.cfg")` 是里面的路径，`EnsureAppDir("Settings")` 建好它。`Settings/` 和 `Logs/` 是约定的子文件夹，其余随固件安排。确认 `IsMounted()` 后，可使用标准 C/C++ 文件接口：

```cpp
#include "storage/app_dir.h"
#include <cstdio>

void ReadSettings()
{
    namespace Storage = CardenzaHAL::Storage;
    if (!Storage::IsMounted()) return;  // 没有卡：照常运行
    FILE* file = std::fopen(Storage::AppPath("Settings/app.cfg").c_str(), "r");  // /sdcard/Apps/<AppName>/Settings/app.cfg
    if (!file) return;
    char line[128];
    while (std::fgets(line, sizeof(line), file)) {
        // Parse a line in your application.
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) Storage::CheckAfterIoError();
}
```

**以前把文件放在别处的固件**（卡根目录下自己的文件夹）在启动时、读任何文件之前调用一次 `Storage::MigrateAppDir("/sdcard/<旧文件夹>")`：只有旧文件夹时改一次名；两个都在时（用户退回旧版本又升级回来），新文件夹没有的移进去，两边都有的留在原处，绝不覆盖；任何时刻断电都不丢东西，下次启动接着做完。细节和旧版本之后看到什么见 [docs/SD-CARD.zh-CN.md](docs/SD-CARD.zh-CN.md#搬走旧文件)。

`GetCapacity()` 返回以**字节**为单位的总容量，不是剩余空间。`IsMounted()` 是缓存状态，不是硬件插卡检测。I/O 出错后，先关闭受影响文件，再调用 `CheckAfterIoError()`：卡仍正常响应时保持挂载（例如只是写满），失去响应时标记为不可用，并在下一次 `Init()` 时清理旧挂载。重试前须协调所有文件使用者；HAL 不提供自动热插拔管理。

## HOME 键、I2C 与红外

- [`System::GetHomeButton()`](system/system.h) 在 **GPIO0 的 HOME 键**（屏幕左侧的侧键）被按下时返回 `true`。它是独立按键，与矩阵键盘及其中的 Fn 键分开。**按一下它就打开系统菜单**（见下节）：打开或关闭菜单的那一下按键在这里读作松开。
- `System::GetI2CBus()` 返回 `System::Init()` 创建的原生 I2C 主总线句柄。用 `i2c_master_bus_add_device()` 添加设备，避免在同一组引脚重复创建总线（SCL GPIO1、SDA GPIO2）。
- `System::EnterDeepSleep()` 当前**只删除共享 I2C 总线**，并不会让 ESP32 进入深度睡眠，也不会配置唤醒源。调用前需要协调编解码器和其他 I2C 使用者。
- [`IR::Send(addr, cmd)`](ir/ir.h) 在 GPIO44 上用 38 kHz 载波发出 8 位 NEC 地址/命令及各自反码。必要时会自行初始化发射器，并等待发送结束。没有接收接口，也不返回发送状态。

## 系统菜单（任何固件里按 HOME）

在 HAL 上的任何固件里，按一下 HOME 侧键（GPIO0），弹出[系统菜单](system/menu.h)。它就是 203 Launcher 的列表：用 Launcher 自己的代码画，语言跟随 Launcher 的设置，两者看起来、动起来是同一个东西。

| 项 | 作用 |
| --- | --- |
| 继续 | 回到固件（ESC、del 也可以） |
| 设置 | 这块板子的设置（见下）：输出音量、音量上限、屏幕亮度、按键灯亮度、语言、恢复默认 |
| 返回启动器 | 打开 Launcher 主页，不管它怎么设置（用它的重启票据：不走“启动上次的软件”倒计时） |
| 重新启动 | 重新启动这个固件（在 Launcher 下用它的启动票据，不倒计时） |
| 关机 | 深度睡眠：屏幕、背光和键灯全关；按 HOME 开机，固件重新启动 |
| 本机信息 | 固件和它的版本、Launcher 的版本、HAL 的提交、MAC、Flash、PSRAM、SD 卡 |
| 下载模式 | 进入 ROM 下载模式，用 USB 刷机（esptool） |

设置页的样子是 Launcher 的“显示”页：每个设置一行，值在右边。在一个值上按 ENTER，打开 Launcher“背光”页那样的一页（大字的值，下面一条刻度；`;`、`,` 减小，`.`、`/` 增大，实时生效：音量马上听得到，亮度马上看得到；ENTER 保留，ESC 恢复原来的值）；语言是 Launcher 的语言列表，每种语言用它自己的文字写；网页提示（插上电脑时 Chromium 内核的浏览器弹出的 WebUSB 落地页提示：仅第一次、总是、从不；默认“仅第一次”，新板子只在第一次接电脑时提示一次；即 [docs/SETTINGS.md](docs/SETTINGS.md) 的 `webusb_landing`）也是同样的三项列表，从电脑下一次找到板子起生效；恢复默认先用 Launcher 的确认框问一句（默认选中“取消”），语言不变。改过的设置在菜单关闭时写进 Flash，只写一次。

不用菜单时：开机或复位时按住 fn（键盘左边一列的第三个键，键号 29）进入 Launcher；不是 HOME：复位时按住 HOME 会进入 ROM 下载模式。

按键和 Launcher 一样：`;`、`,` 向上，`.`、`/` 向下，ENTER 或单击 HOME 确定，ESC 或 del 返回；菜单打开时这几个键会亮。

屏幕上和 Launcher 一模一样：同样的灰度（只有黑、白、灰：菜单显示的每个像素在面板上红、绿、蓝都相等，背景也一样），同样的亮度（菜单显示期间用板子的屏幕亮度，也就是 Launcher 的背光设置，它 NVS 里的 `launcher/bright`，按它的占空比曲线和它的 256 Hz PWM，不管固件自己设的亮度是多少；关闭后恢复固件的亮度）。

出现方式和动效（这块屏没有防撕裂信号线，任何一帧都可能在扫描中途写进去：会动的东西每帧只变几个像素，盖住整屏的东西一个像素一个像素地换）：

- **按一下就打开。** HOME 按下满 30 ms（过了触点抖动）算一次按键；固件启动时就按着的 HOME、或者菜单关闭时还按着的 HOME，要先松开才算。固件的画面（它的最后一帧）在一帧里变暗、变灰：亮度取五分之二，经 4×4 有序 dither 变成五级暗灰。70 ms 后菜单按 dither 的顺序透出来，每帧换四分之一的像素，背光同时变到 Launcher 的亮度：每个像素要么是背景的、要么是菜单的，从不混色，所以屏上没有任何颜色，一帧在扫描中途写进去也看不出接缝。
- **菜单里**深色的选中条靠上下两条边滑过去，和 Launcher 的列表一样（它的 `ListSlide`）：朝移动方向的那条边先走（Launcher 的缓出曲线，150 ms），另一条从静止开始跟上（230 ms），所以选中条先朝新的一行拉长，再缩回到这一行的高度；滑动途中再按，从两条边当时的位置接着走。文字在边经过的地方逐像素变白或变黑，一行的说明随选中条盖住它而展开。翻过最后一行时，各行一下子换好，选中条从它在屏上的位置接着滑。滑动时一帧画完，隔一个 tick 就画下一帧（有 PSRAM 时每帧在 PSRAM 里只画一次；没有时每 10 ms 一帧），每 8 行一条带子只发送变了的部分，也就是边和文字扫过的那几行、那几段 30 像素：平均每帧约 1-3 KB，最多 16 KB；以前整条带子发送，平均 3-9 KB，最多 35 KB（`tests/system_menu_test.cpp` 对 `--compare` 的每一段滑动都量了一遍）。
- **继续**：先显示固件最新一帧的暗灰背景 60 ms，再显示固件的画面，之后固件自己的帧接着画。**其他项**：屏幕像 Launcher 关机时那样渐暗（600 ms）后再执行。

- 屏幕初始化成功后由 `CardenzaHAL::Init` 启动（`CONFIG_CARDENZA_HAL_SYSTEM_MENU`，默认开）：一个 5 KB 栈的任务，每 10 ms 读一次 GPIO0；约占 30 KB flash。菜单打开期间临时借用 2 KB 内存，固件有 PSRAM 时再在 PSRAM 里借 64 KB 存一份固件最新的画面（背景是用它做的；没有 PSRAM，或者按下后 60 ms 内固件没有提交过一帧时，背景是一片暗的 dither 灰），和 32 KB 的菜单画面（每帧画一次；没有 PSRAM 时逐条画）；菜单打开期间再借一张 4 KB 的表，记着已经发出去的画面（有 PSRAM 时放在 PSRAM 里）。
- 菜单打开期间固件照常运行（任务和声音都不停），但 HAL 会挡住会干扰菜单的东西：`Display::Present*` 不发送并返回 `true`（菜单关闭后的下一帧整屏发送，固件画面自己回来）；`Keyboard::updateKeyList` 不报任何按键（关闭时还按着的键，松开之前都不报）；按键灯显示菜单的键（固件 `setLED*` 设的亮度会保留，关闭后恢复）。直接用面板句柄画屏的固件，应在 `SystemMenu::IsOpen()` 时暂停绘制。
- `SystemMenu::SetListener` 在菜单打开和关闭时通知；`SystemMenu::SetLeaveHook` 在重启、关机或进入下载模式之前调用（保存需要留下的东西）。菜单开着时 HOME 归菜单：自己要用 HOME 的固件，调用 `SystemMenu::Enable(false)`（或者编译时关掉 `CONFIG_CARDENZA_HAL_SYSTEM_MENU`）。
- Launcher 的代码在 [system/menu/launcher/](system/menu/launcher/)，由 `tools/launcher_ui.py --launcher <203-launcher 检出目录>` 原样复制（MIT，[licenses/203-launcher-LICENSE.txt](licenses/203-launcher-LICENSE.txt)）：文字和字形、画布绘制、缓动和选中条的滑动；[system/menu/frame.cpp](system/menu/frame.cpp) 是它在 Cardenza 上的 `menuFrame` 和侧栏，逐行移植。`--check` 检查复制是否最新，`--compare` 再用 Launcher 真正的 `menuFrame` 和 `drawDialog` 画每一帧：静止的画面（主列表、设置页、语言列表、恢复默认的确认框、网页提示的三项），加上逐帧的滑动（移一行、连按、中途折返、往下和往上翻过一屏，每 4 ms 一帧直到停稳：五种语言共 2005 帧），有任何一个像素不同就失败（主机测试里存着这些帧的哈希）。数值页和本机信息页是 Launcher“背光”页和“关于”页那几行的移植（Launcher 是在别的函数里直接画的）。文字在 `system/menu/strings.json`。
- 中文、日文和越南文用的是 203 Launcher 里由 **HarmonyOS Sans**（12 像素，恢复默认的确认框用 16 像素粗体）生成的点阵，由同一个工具只为菜单的文字从 Launcher 的字形表里取出（Launcher 的字形表里还没有的字，比如“默”，由 Launcher 自己的 `tools/i18n.py` 用它指定的字体生成：`--fonts`）；点阵是用该字体制作的作品，授权允许分发（[licenses/HarmonyOS-Sans-LICENSE.txt](licenses/HarmonyOS-Sans-LICENSE.txt)），仓库里不含字体文件。5x7 字体来自 NullPerator（BSD-3-Clause，声明见 [system/menu/launcher/font5x7.h](system/menu/launcher/font5x7.h)）。

## 板子的设置（所有固件通用）

[`system/settings.h`](system/settings.h)：输出音量（0–100，按听感，不超过音量上限）、屏幕亮度（就是 203 Launcher 自己的设置）、按键灯亮度和语言属于这块板子，由 HAL 存在板子的 Flash 里，在系统菜单（HOME > 设置）或通过 USB（`203cli board settings`）修改。`CardenzaHAL::Init` 在固件的第一帧之前就把它们用上；固件启动时不要自己设音量、背光或按键灯总亮度（那样会盖过用户的设置）。用 `Settings::Get` 读，用 `Settings::Set` 做自己的快捷键（马上生效，一秒后写入），麦克风增益不在其中：它是固件自己的，录音时交给 `Mic::ApplyGain`。Flash 只由 HAL 写，从不每改一次写一次，写的时候先把喇叭淡出、按键灯暂时熄灭：[docs/SETTINGS.md](docs/SETTINGS.md)。

## USB 设备用法

`CardenzaHAL::Init()` 会启动 USB 设备（Kconfig `CONFIG_CARDENZA_HAL_USB_AUTOSTART`，默认打开），默认的类是 MIDI、双向音频、HID（键盘、媒体键）和 WebUSB，每个都带系统通道，所以任何固件都能被 203 社区网站和 `203cli` 识别、送去烧录，不需要人按任何键。它一秒后才连接：包含 `usb/usb.h`，在 `CardenzaHAL::Init()` 之后紧接着调用 `USB::Init(config)` 选你自己的类，电脑就只会看到你的。之后再 `Init`，或者 `USB::Reconfigure(config)`，电脑会重新找到带新类的设备。只需要 MIDI 应用接口时：

```cpp
#include "usb/usb.h"

CardenzaHAL::USB::Config config;
config.product = "My Cardenza App";   // 固件自己的名字，在 IDENTIFY 和 INFO 里（USB 名字仍是 "Cardenza"）
config.audio = false;
config.keyboard = false;
config.consumer = false;
// config.fallbackSeconds = 30;  // Optional development recovery; leave 0 in products.
bool started = CardenzaHAL::USB::Init(config);
```

设备永远留着一条能被识别、被烧录的路：不管关掉什么，HAL 都会保留 WebUSB（不占端点），或者加上串口。固件已经调用过、开发恢复机制跳过本次启动，或启动失败时，`Init()` 返回 `false`。可分别检查 `Started()` 和 `Mounted()`；后者表示已被主机配置且未挂起。`Classes()` 说明设备实际提供了哪些类。

| 类（`Config`，Kconfig `CONFIG_CARDENZA_HAL_USB_*`） | IN 端点 | 带系统通道 | 默认 |
| --- | --- | --- | --- |
| MIDI（`midi`） | 1 | 是，用 SysEx | 开 |
| 音频（`audio`）：电脑录设备的声音，也向设备播放（`audioPlayback`） | 1，播放的反馈端点再加 1（`audioFeedback`） | 否 | 开，带反馈 |
| HID（`hid`）：键盘、媒体键、手柄、原始报告 | 1 | 是，报告 0xCB | 开 |
| WebUSB（`webusb`）：给网页和电脑程序用的厂商接口 | 0 | 是，控制请求 | 开 |
| 串口（`serial`，Kconfig `CONFIG_CARDENZA_HAL_USB_SERIAL`）：CDC | 2 | 是，按行，外加 esptool 的复位 | 关（没有别的类带系统通道时自动加上） |
| U 盘（`USB::Drive`）：SD 卡 | 1 | 否 | 运行时再要 |
| 摄像头（`USB::Camera`）：屏幕的无损画面流，在 WebUSB 接口上 | 1 | 否 | 运行时再要 |
| 网络（`USB::Network`，Kconfig `CONFIG_CARDENZA_HAL_USB_NETWORK`）：CDC-NCM | 2 | 否 | 运行时再要 |

ESP32-S3 除 EP0 外有四个 IN 端点（IN 端点的 TX FIFO 就是它编号的那个，所以编号只能是 1 到 4），默认的类正好占满。默认打开的类放不下时，编译直接停下并说明该关哪个（`usb/config/tusb_config.h`）。固件借出 SD 卡或打开摄像头时，端点从下面这些里按顺序让出来：播放的反馈端点（播放改为自适应：HAL 偶尔丢一帧或重复一帧，不超过万分之一）、HID、串口、音频、MIDI，每一项都只在还有别的类带着系统通道时才让。日志会写出让了什么。

| 接口 | 数据与应用责任 |
| --- | --- |
| `MidiRead(packet)` / `MidiWrite(packet)` | 虚拟线缆 0 的四字节 USB-MIDI 1.0 事件包，不是三字节原始 MIDI 流。例如 `{0x09, 0x90, 60, 100}` 表示通道 1 Note On；检查返回值。 |
| `AudioWrite(buffer, frames)` | 44.1 kHz、左右交错的立体声 `int16_t`，发给主机录音 |
| `AudioRead(buffer, frames)` | 读取主机播放流，返回实际读取的立体声帧数 |
| `KeyboardReport(modifiers, keys)` | 六个 HID 键码与修饰键位；发送全零报告以释放按键 |
| `ConsumerReport(usage)` | 一个媒体功能 usage，`0` 表示释放 |
| `SetAppReportHandler(handler)` / `SendAppReport(data, length)` | 最多 `kReportSize`（63）字节的应用报告；回调在 USB 任务中执行，应保持简短 |
| `Reconfigure(config)`、`CurrentConfig()`、`Classes()` | 运行中换类；固件要的配置；设备实际提供的类 |

USB 音频长度单位是**帧**，每帧两个采样，共四字节。每个音频块都应服务收发并处理短传输。主机没有录音时，`AudioWrite()` 会丢弃数据但仍返回请求帧数；需要区分时检查 `AudioInputActive()`。`AudioOutputActive()` 表示主机是否正在播放。

USB 音频不会自动接到扬声器或麦克风，应用需自行桥接，包括声道转换及共用引脚管理。需要响应主机音量时，由应用读取并应用 `HostMuted()`、`HostVolume()`；后者单位是 1/256 dB，`0` 表示满电平。`AudioStats()` 提供端点计数和以字节为单位的 FIFO 占用量。

### USB 名字：每种模式都是同一台设备

| 板子的状态 | VID:PID | 厂商 / 产品 | 序列号 |
| --- | --- | --- | --- |
| 跑 HAL 固件（协议 4） | 0203:10C0（或固件的 `productId`） | 203 Systems / Cardenza | MAC，`70:AF:09:BC:EB:30` |
| 203 Launcher，以及 ROM 下载模式（USB-Serial-JTAG） | 303A:1001 | Espressif / USB JTAG/serial debug unit | 同一个 MAC，同样的写法 |
| 203 Launcher 的 U 盘模式（它的 `feat/usb-everywhere` 起） | 0203:10CF | 203 Systems / Cardenza | 同上 |

电脑靠序列号在不同模式之间认出同一颗芯片（Web Serial、WebHID、Web MIDI 看不到序列号：协议的回答里有 `chip.mac`）。各接口的名字说明它是什么："Cardenza MIDI"、"Cardenza Audio"、"Cardenza HID"、"Cardenza WebUSB"、"Cardenza Serial"、"Cardenza Storage"。固件自己的名字和版本在 IDENTIFY 和 INFO 里，不在 USB 名字里（协议 4 之前，产品名是固件的 `Config::product`，序列号不带冒号）。

### HID 输入：键盘、媒体键、手柄

[`usb/hid.h`](usb/hid.h) 让 Cardenza 当电脑的键盘、媒体键和游戏手柄。它们是 USB 设备唯一一个 HID 接口里的报告 ID（键盘 1、媒体键 3、手柄 4，与 Matrix OS 相同），和系统通道（0xCB）、固件自己的原始报告（0xFF）在一起，不占自己的端点。设备带哪几样由 `USB::Config` 决定：`keyboard` 和 `consumer`（默认开），`gamepad`（默认关；在 `CardenzaHAL::Init` 之后紧接着用自己的 `USB::Init` 配置打开）。[`examples/usb_hid`](examples/usb_hid) 用 Cardenza 的按键演示这三样。

| `CardenzaHAL::USB::Hid::` | 作用 |
| --- | --- |
| `Press(usage)` / `Release(usage)` / `ReleaseAll()` / `Tap(usage)` | 按住和松开按键：0x07 页的 usage（`kKeyA`、`kKeyEnter`、`kKeyLeft` 等），修饰键 0xE0-0xE7（`kKeyLeftShift`，`kKeyLeftGui` 在 Mac 上是 Command）。同时最多六个键加修饰键，第七个不收。 |
| `Type(text, timeoutMs)` | 按美式键盘打出可打印 ASCII（`\n` 回车，`\t` Tab），每秒约 500 个字符；打字时按住的键先抬起，打完再按回去。 |
| `KeyboardLeds()` | 电脑设定的键盘灯（`kLedCapsLock`、`kLedNumLock`、`kLedScrollLock`）。 |
| `Media(usage)` / `MediaPress(usage)` / `MediaRelease()` | 一个媒体键（`kMediaPlayPause`、`kMediaNext`、`kMediaVolumeUp`、`kMediaMute` 等）。 |
| `SetGamepad(state)` / `GamepadState()` / `HatFrom(up, down, left, right)` | 六个轴（-127..127）、一个方向帽和 32 个按钮。 |
| `Ready()` | 电脑已连上设备和它的 HID 接口。 |

HAL 记着按住了什么，电脑最终看到的总是这个状态：报告一时没地方或没有电脑（拔掉、睡眠）时，能发时立刻补发；电脑回来（唤醒、重新插上）时先收到当前状态。电脑离开、或设备的类改变时，一切都会松开，不会有键一直按着。各函数返回这次的报告是否已经在路上。在 macOS 上，HID 键盘会让电脑问一次这是哪种键盘，之后主机工具只有在“输入监控”权限下才能打开 HID 接口（系统通道也一样；SysEx、WebUSB 和串口都不需要权限）：固件不打字时把 `keyboard` 关掉。上面的 `KeyboardReport`、`ConsumerReport` 仍然直接发原始报告；二者选一，不要混用。网页模拟器里没有电脑：这些函数返回 false。

### USB 协议与平台说明

报告号和指令沿用 Matrix OS：报告 0xCB 是系统指令（0x40 Launcher / 更新模式，0x41 重启，0x42 ROM 下载模式，0x43 控制台，0x44 识别，0x45 [开发通道](#开发通道203cli-board)；应答为指令 | 0x80），同样的指令也可以用 SysEx `F0 23 <指令> F7`、WebUSB 控制请求和串口的 `sys <hex>` 行发（[`docs/usb-dev.md`](docs/usb-dev.md) 的 "Transport"）。固件自己的报告（0xFF）在电脑没法用 HID 时（比如用 Web MIDI 的网页）也能用 SysEx `F0 23 7F <字节> F7` 发，`SendAppReport` 按上一条来的方式回复。固件的 MIDI 包和 HAL 的 SysEx 一次只有一方在写，SysEx 到电脑时是完整的。VID 0x0203（203 Systems）。

- 它占用芯片唯一的 USB PHY：控制台转到系统通道（有串口时也到串口），esptool 的串口没有了，直到重启进 Launcher 或 ROM 下载模式。要在 USB-Serial-JTAG 上用 JTAG 调试或 `idf.py monitor`，关掉 `CONFIG_CARDENZA_HAL_USB_AUTOSTART`，也不要调用 `USB::Init`。
- 有串口时，它回答 203 Launcher 控制台同样的词（`version`、`info`、`partitions`、`status`、`launcher`、`download` 等：[`docs/usb-dev.md`](docs/usb-dev.md) 的 "The serial port's words"），并在收到 esptool 的 DTR/RTS 复位或 Arduino 的 1200 波特率触碰时重启进 ROM 下载模式；板子随后作为 USB-Serial-JTAG 出现，是一个新的串口。
- WebUSB：BOS 里写了落地页（`CONFIG_CARDENZA_HAL_USB_WEBUSB_URL`，stage.203.io），也给 Windows 提供 Microsoft OS 2.0 描述符，WinUSB 直接接管这个接口，不用装驱动。Linux 上 Chrome 要对这个 USB 设备有写权限（一条 udev 规则）；其他类不需要。
- TinyUSB 只由 `usb/config/tusb_config.h` 配置。同一个固件里不要再加 `esp_tinyusb` 或其他配置 TinyUSB 的东西。
- 内存：默认的类静态约 8.5 KB（音频 FIFO 每个方向 2 KB），WebUSB 队列再加 1 KB，有串口加 2 KB，有 U 盘加 4 KB；启动后再加 4 KB 任务栈。
- `keyboard = true` 时，macOS 会请用户识别一次键盘，电脑上的工具也只有在"输入监控"权限下才能打开 HID 接口（包括系统通道）。SysEx 和 WebUSB 不需要权限。
- 已在 macOS 上实测（2026-11-02）：MIDI 双向、HID 和 SysEx 系统指令、控制台、44.1 kHz 双向音频。全速下反馈值按 10.14 发 3 字节（TinyUSB 默认的 16.16 会让 macOS 在 128 ms 后停播）；请求 Microsoft OS 描述符的主机（字符串 0xEE，或有 WebUSB 时的 MS OS 2.0 描述符集）收到 16.16（Windows，未测试）。TinyUSB 的 DWC2 驱动不会重启错过帧的同步 OUT 传输；HAL 在 20 ms 收不到数据时重启播放端点。
- 乐鑫的 QEMU（社区的开机检查）里没有 USB 控制器：`Init` 干净地失败，控制台留在串口上。

### U 盘：电脑上的 SD 卡

[`usb/drive.h`](usb/drive.h) 把卡借给电脑，当作一个可移除磁盘（"Cardenza Storage"，SCSI 名 "203 Cardenza SD"），不用拔卡就能拷文件。需要打开：`CONFIG_CARDENZA_HAL_USB_DRIVE`（CardenzaHAL > USB；缓冲约 4 KB 内部 RAM，借出期间另有一个 6 KB 栈的任务）。只有借出期间 USB 设备里才有这个磁盘：电脑会重新认一次设备（MIDI 端口和音频流重新开始），磁盘用的 IN 端点从让路的类里来（先让放音的反馈端点：放音变成自适应，什么也不丢）。

```cpp
#include "usb/drive.h"

// 每次状态变化都会告诉你（在 U 盘的任务里）：听到 Starting 时关掉你在卡上打开的文件。
CardenzaHAL::USB::Drive::SetListener([](const CardenzaHAL::USB::Drive::Status& status) {
    if (status.state == CardenzaHAL::USB::Drive::State::Starting) CloseMyFiles();
});
bool shared = CardenzaHAL::USB::Drive::Share();  // 最多等 3 秒，直到磁盘出现
// ……之后
CardenzaHAL::USB::Drive::Stop();                 // 卡回到固件手里
```

- 同一时间只有一方用卡。借出期间卡的文件系统是卸载的：`Storage::IsMounted()` 为假，`Storage::LentToComputer()` 为真，`Storage::Init()` 和 `/sdcard` 下的所有文件操作（包括 `AppDir` 那些函数）都会失败，固件还开着的文件会返回 `EBADF`。在监听函数收到 `State::Starting` 时关掉文件：它一返回，文件系统就放手。
- 卡在这些时候回来（磁盘从电脑上消失，文件系统重新挂载，监听函数收到 `State::Off`；挂不上时是 `Failed`，原因 `CardError`）：调用 `Stop()`；电脑弹出磁盘；主机取消配置设备（拔线）；主机离开 10 秒（睡眠，或者自带电源的板子被拔线，ESP32-S3 只看到总线挂起）；`Share()` 之后 15 秒内没有电脑用这个磁盘。没有已配置的主机（`NoHost`）、USB 没启动（`NotStarted`）、没卡（`NoCard`）、USB 上腾不出端点（`NoRoom`）、固件没编进 U 盘（`NotBuilt`）时，`Share()` 立刻失败。
- 电脑写的内容先写进卡，再告诉电脑写完了（设备上没有缓存）。电脑自己缓存里还没写下去的就不在卡上：像任何 U 盘一样，先弹出再拔；写到一半拔线，电脑正在写的文件会被截断，FAT 停在电脑写到的地方（电脑的磁盘检查能修好）。
- USB 任务负责在端点和卡之间搬每一个 4 KB 的块（几毫秒；卡写得慢时可达几百毫秒）：这期间 MIDI、HID 和系统通道要等，USB 音频靠它的 FIFO 照常走。
- `examples/usb_drive`：按一个键借出卡、再按一下收回，屏幕显示状态。

### 画面流（摄像头）

专为录制和直播教程而做：[`usb/camera.h`](usb/camera.h) 把屏幕作为无损画面流交给电脑，RGB565 像素完全准确、没有有损环节，只发送自电脑读完上一帧以来变化的部分（原样或简单的游程编码），走 "Cardenza WebUSB" 接口上的一个 bulk 端点。网页可以把它和设备的 USB 音频一起显示、录制；桌面程序可以把它做成 OBS 用的虚拟摄像头（Windows 用 WinUSB，无需驱动；macOS 和 Linux 用 libusb）。协议见 [`docs/screen-stream.zh-CN.md`](docs/screen-stream.zh-CN.md)；参考读取实现 [`tools/web/screen-stream.js`](tools/web/screen-stream.js)；测试固件 [`examples/screen_stream`](examples/screen_stream)，配合 [`tools/web/screen-stream-demo.html`](tools/web/screen-stream-demo.html)。

```cpp
// Kconfig CONFIG_CARDENZA_HAL_USB_CAMERA=y（需要 WebUSB）；然后从第一次枚举起就带摄像头：
CardenzaHAL::USB::Config usb;
usb.camera = true;
CardenzaHAL::USB::Init(usb);
// 或者之后再打开（电脑会重新识别设备）：CardenzaHAL::USB::Camera::Enable(true);
```

- 显示的是固件用 `Display::Present(framebuffer...)` 或 `PresentChanged` 提交的画面：直接通过面板句柄绘制的固件看不到，系统菜单也不在其中。
- 电脑开始读取之前零开销。读取时：130 KB PSRAM、一个优先级 1 的任务（栈 3.5 KB），以及每次 `Present` 时把提交的行和屏幕副本比较。占一个 IN 端点，从让位的类中取得（首先是音频回放的反馈端点：回放改为自适应）。
- 普通页面的关键帧 8-27 KB（原样 64.8 KB）；界面变化每秒几十 KB；电脑比固件慢时，变化合并成更少的帧，不会积压。

### USB 网络：板子当网卡

[`usb/network.h`](usb/network.h)（Kconfig `CONFIG_CARDENZA_HAL_USB_NETWORK`，按需打开）让板子成为电脑的一块有线网卡（CDC-NCM），跑在 lwIP 上，和 HAL 的 Wi-Fi 是同一个协议栈：ESP-IDF 的 `esp_http_server`、socket、`esp_http_client` 都和在 Wi-Fi 上一样用。`Network::Start(mode)` 加上这个类（电脑会重新找到设备；它要的两个 IN 端点从让出来的里面拿：先是播放的反馈端点，再是 HID），`Stop()` 去掉它。[`examples/usb_network`](examples/usb_network) 在 <http://cardenza.local> 提供一个显示板子信息的网页。

| 模式 | 板子 | 电脑 | 上网 |
| --- | --- | --- | --- |
| `Mode::Device`（默认） | `192.168.203.1/24`，DHCP 服务器，`<hostname>.local`（`cardenza.local`）的 mDNS | 从 `192.168.203.2` 起租地址：租约里没有路由器，也没有 DNS 服务器 | 电脑的网络照旧（什么都不经过板子） |
| `Mode::Uplink` | 链路上的 DHCP 客户端 | 要在电脑上为这个链路打开网络共享（macOS 的互联网共享、Windows 的 ICS、Linux 的 NAT） | 板子通过电脑上网 |

- **和 Wi-Fi 并存**（[Net](#网络wi-fi-与-https)）：Wi-Fi 连着时它是默认路由；Wi-Fi 不在时由 USB 上行接替（`Options::preferOverWifi`：上行优先）。DNS 服务器永远是默认接口的：这个选项会打开 ESP-IDF 的 `CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF`，USB 链路上的租约不会顶掉 Wi-Fi 的 DNS，任何一边断了 Net 都照常工作。设备模式永远不当默认路由（没有网关，优先级最低）。
- **mDNS**：HAL 自己在 lwIP 的 UDP 上写的应答器（约 1 KB Flash，不用加组件，不碰其他接口）：只在 USB 链路上回答 `<hostname>.local` 的 A 查询（组播；一次性查询回到它的端口，TTL 10 秒），链路连上时宣告三次。不回答 AAAA，也没有服务记录：`http://cardenza.local` 和 `ping cardenza.local` 都能用。一台电脑接两块板子：给它们不同的 `Options::hostname`。
- **链路**：跟着线缆、电脑睡眠和设备的类变化走（`GetState().link`：`Waiting`、`Up`、`Ready`；`SetListener`）。每次变化、电脑租到的地址、上行拿到的租约都写在日志里（`usb-net:` 开头）。换一种模式重新启动时，会告诉电脑线拔掉又插上，让它重新要地址。
- **电脑**：macOS、Linux 和 Windows 11 自带 NCM 驱动；Windows 10 没有（看得到接口，没有驱动）。Chrome OS 和 Android 用 Linux 的驱动。
- **开销**：编进这个类时静态内部 RAM 约 6.6 KB（NCM 的收发块，每个方向 3.2 KB；2026-11-24 在模板上量的），lwIP 之外约 3 KB Flash（在线服务本来就链接了 lwIP；关掉它的固件还要算上 lwIP 和 esp_netif）。启动后：一个任务（3 KB 栈）、四个发送槽（6 KB，有 PSRAM 就放 PSRAM）和每个包 lwIP 的 pbuf。电脑来的帧在 USB 任务里拷进 pbuf 交给 lwIP，从不等待；发给电脑的帧最多等 10 毫秒空出的槽。
- 速率：全速 USB，最多约 1 MB/s，和其他类共用。

### 开发通道（`203cli board`）

HAL 上的每个固件，不用写一行代码，就能回答电脑查询它自己的情况：系统通道上的指令 0x45（协议见 [`docs/usb-dev.md`](docs/usb-dev.md)），电脑上用 [203-CLI](https://github.com/203-Systems/203-CLI) 的 `203cli board` 读：

| `203cli board` | 显示或做什么 |
| --- | --- |
| `info` | 固件（名字、版本、社区代码、ESP-IDF、编译时间、解崩溃栈要用的 ELF SHA-256）、HAL 的提交、203 Launcher 的版本、芯片（版本、MAC、Flash、PSRAM）、开机多久、上次为什么重启 |
| `partitions` | 板子上的分区表，每个应用镜像占它分区的大小，NVS 用量，剩下的 Flash |
| `memory` | 内部 RAM、PSRAM、能做 DMA 的 RAM（剩余、最低剩余、最大连续块）；每个任务的优先级、核心、栈最少剩多少，以及它占的 CPU |
| `storage`、`status`、`i2c` | SD 卡和剩余空间；屏幕、按住的键、系统菜单、USB、在线服务；I2C 总线（扩展口那条）上的设备 |
| `screen` | 固件画面的截图（PNG） |
| `press` | 从电脑按键（包括 HOME），固件当作真的按键 |
| `coredump` | 上一次崩溃的转储：崩在哪个任务、调用栈、转储文件本身 |
| `files` | SD 卡上的 `ls`、`get`、`put`、`rm`、`mkdir`、`mv` |
| `settings` | 板子的设置和它们的范围；`get`、`set`、`reset`（关掉开发通道时也能用） |

- 电脑不问就没有任何开销：没有任务、没有缓冲、不轮询。来了查询才建一个优先级 1 的任务（内部 RAM 里 6 KB 栈，最后一次查询 10 秒后删掉）；截屏在固件下一次 `Present` 时往 PSRAM 拷一帧。Flash 都经缓存读，从不关缓存。约 20 KB Flash、0.4 KB 静态内部 RAM（2026-11-20 在启动了 USB 的模板上量的）；关掉 `CONFIG_CARDENZA_HAL_USB_DEV` 就整个不链接。
- 从不发设备令牌，碰不到 SD 卡以外的文件。只做被要求的写：文件（`files put`、`rm`、`mkdir`、`mv`）、擦掉崩溃转储，以及改一项设置（和设置的每次改动一样写入：[docs/SETTINGS.md](docs/SETTINGS.md)）。
- 任务列表和 CPU 占用要 `CONFIG_FREERTOS_USE_TRACE_FACILITY` 和 `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`，崩溃转储要 `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`：板子的调试配置（[`board/sdkconfig.debug`](board/sdkconfig.debug)）都打开了；正式版不开，省下约 5 KB 内部 RAM，其余功能照常可用。
- 截屏要固件用帧缓冲显示（`Present`、`PresentChanged`）；直接用屏幕句柄画的固件没有帧缓冲。
- 每个固件都有：`CardenzaHAL::Init` 会启动 USB。它在每个带系统通道的类上都回答（HID、SysEx、WebUSB、串口）；关掉 `CONFIG_CARDENZA_HAL_USB_DEV` 时 INFO 也照样回答。

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
| GPIO0 | HOME 键（HOME；按一下打开系统菜单）与 ROM 启动选择 |
| 内部 USB PHY | USB 设备或 USB-Serial-JTAG |
| Wi-Fi 射频 | Net：固件在 `Connect` 和 `Disconnect` 之间，以及在线服务运行期间（固件自己运行 Wi-Fi 之前先 `Presence::Enable(false)`） |

## 板子默认配置、分区表与刷机

HAL 附带了这块板子本身的事实，作为固件可以直接用的默认值（都不强制）：

- [`board/sdkconfig.defaults`](board/sdkconfig.defaults)：16 MB 闪存，四线 120 MHz（和 203 Launcher 的 bootloader 一样）；2 MB PSRAM，120 MHz；FreeRTOS 1000 Hz（延时和 `FramePacer` 的拍子可以精确到毫秒；屏幕已经不依赖它）；FAT 用 512 字节扇区和长文件名。在 `SDKCONFIG_DEFAULTS` 里把它放在最前，自己的默认值放后面：
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

- [`tools/cardenza.py`](tools/cardenza.py)：首选的编译方式。`doctor` 说出项目和电脑还缺什么（ESP-IDF 5.3、git、干净的 HAL 子模块、`203cli` 及其登录、Emscripten）以及怎么装；`build` 先检查，再运行 `idf.py build`（shell 没激活时自己导出 ESP-IDF），并说出镜像占了 Launcher 空间的多少（`--web` 同时编译网页版）；`web` 编译网页版；`flash` 和 `log` 交给 `203cli flash --bin` 和 `203cli log`。只用标准库，Linux、macOS、Windows 都能用。

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
- [`docs/FIRMWARE-PROJECT.md`](docs/FIRMWARE-PROJECT.md)：基于 HAL 的固件工程规则，以及怎样用 `203cli` 发布到 203 社区（社区在 `/develop.md` 提供同一个文件）。
- [`examples/template`](examples/template)：做新固件的起点：HAL 作为子模块、板子默认配置、保留 Launcher 的分区表、`version.txt`、`CHANGELOG.md` 和 `203.json`；同时能编译到板子和网页。
- [`examples/psram_heap`](examples/psram_heap)：一个小固件，把 PSRAM 加进堆、只给明确申请的代码用，并替换 `new`，让申请失败变成空指针而不是重启。`idf.py -C examples/psram_heap build`。
- [`examples/launcher_net`](examples/launcher_net)：读出 Launcher 的设置，连上它的 Wi-Fi，发一个 HTTPS 请求再把一切关掉，每一步都打印耗时和内存；它的 `sdkconfig.defaults` 是使用 Net 的固件的参考配置。`idf.py -C examples/launcher_net build`。
- [`examples/community_presence`](examples/community_presence)：带在线服务的 `CardenzaHAL::Init()`、交接钩子、在日志里打印服务的状态。`idf.py -C examples/community_presence build`。
- [`examples/hal_check`](examples/hal_check)：通过 HAL 逐个检查板子：每个键的格子和按键灯、音频任务为每个按住的键发一个音、HOME。同一份源码既能编译到板子上，也能编译成网页版。
- [`examples/usb_network`](examples/usb_network)：板子当 USB 网卡（`usb/network.h`）：<http://cardenza.local> 上有一个实时显示板子信息和链路状态的网页；按任意键切到上行模式（用电脑共享的网络）并通过它取一个网页，再按切回。屏幕颜色就是状态（灰：关，琥珀：等电脑，绿：设备模式就绪，蓝：上行就绪，白条：取网页成功）。`idf.py -C examples/usb_network build`。
- [`examples/usb_drive`](examples/usb_drive)：SD 卡当 U 盘（`usb/drive.h`）：按任意键把卡借给电脑、再按收回，屏幕颜色就是状态（灰：固件在用，琥珀：交接中，绿：电脑在用，红：失败）。`idf.py -C examples/usb_drive build`。
- [`.github/actions/community-publish`](.github/actions/community-publish/action.yml)：每个固件的标准发布，用 `203cli` 和受信任发布把版本发到 203 社区的 GitHub Action（推送到 main 是 dev，`vX.Y.Z-beta.N` 和 `vX.Y.Z` 标签是 beta 和 stable；见[发布](#发布)）。
- [`.github/actions/launcher-publish`](.github/actions/launcher-publish/action.yml)：旧的 Launcher hub，只用于过渡期：还有人在用只读 hub 目录的 203 Launcher 0.2.0，所以 203 Systems 的官方固件把正式版同时镜像到那里。新固件不要用它。
- **AI 代理与 203 社区**：要发布固件版本、读取用户需求、查看检查报告或把固件发到设备，先装一次 203cli 和它的 Agent Skill：`npm install -g 203cli && 203cli skill install`（Skill 装到 `~/.claude/skills/203-community`；说明见 https://community.203.io/agents）。203cli 原名 `203c`：脚本或旧说明里写 `203c` 的，改用 `203cli`。

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

页面铺满窗口（板子 7:4，黑底），电脑键盘按按键位置对应（键位表在 `sim.json` 里，试玩页会显示），也可以用触摸或鼠标按页面上的键（右键锁住一个键）；第一次点击或按键后开始出声；`app_main` 运行后向外层页面发送 `{"cardenza_sim":"ready"}`，接着是 `{"cardenza_sim":"keys","keys":[…]}`；然后是 `{"cardenza_sim":"features","features":["screenshot"]}`；另外还有 `restarted`、`crashed`、`stopped`（10 秒内崩溃三次）、`ir` 和 `app_report`。外层页面可以用 `{"cardenza_sim":"app","data":[…]}` 向固件的应用通道发送数据。**截屏只截屏幕，不带板子和键盘**（社区的图片标准，[cardenza.203.io/community/guide/images](https://cardenza.203.io/community/guide/images)）：板子上的 **SCREENSHOT** 把 240 × 135 的画面按最近邻放大 4 倍存成 960 × 540 的 PNG，**FRAMED** 存成标准图片（1280 × 720：一种纯色背景、#1A1C1E 的边框、屏幕在 (160, 90)）。外层页面用 `{"cardenza_sim":"screenshot","id":…,"framed":true,"color":"#E85A2C"}` 要截图，收到 `{"cardenza_sim":"screenshot","id","framed","color","width","height","name","png"}`（`png` 是 ArrayBuffer；第一帧之前是 `error`）；沙箱里的 iframe 可能不能下载，所以自己保存截图的外层页面发 `{"cardenza_sim":"host","screenshot":true}`，板子上的按钮就隐藏（社区的试玩页就是这样）。用旧版 CardenzaHAL 编出来的网页版没有这些。URL 参数：`?sd=0`（没有卡）、`?sdmb=<MB>`（卡的大小，默认 8192）、`?log=1`（在板子下面显示控制台）。`esp_restart` 和崩溃会在同一页面里启动一个新实例（声音、屏幕和 MIDI 保持）。

固件的 CMake 可以用变量 `CARDENZA_HAL_WEB`（代码里用同名的宏定义）判断是否在编译网页版，只在少数必须的地方用：比如自带网页版、靠 `__EMSCRIPTEN__` 切换的第三方库（`__EMSCRIPTEN__` 仍然定义着：C 库需要它）。

### 什么叫"遵循 HAL"

固件只通过 HAL 的接口和它给出的原生句柄（屏幕的 panel 和 panel IO、I2S 通道、I2C 总线、SD 卡上的文件），以及 FreeRTOS 和 ESP-IDF 的系统服务访问硬件，就是遵循 HAL。网页目标提供的正是这些，行为与设备一致：

| 范围 | 网页目标上 |
| --- | --- |
| FreeRTOS | 任务、优先级、两个核（`xTaskCreatePinnedToCore`）、按 tick 的延时、队列、信号量、带优先级继承的互斥量、队列集、任务通知、事件组、软件定时器、流缓冲和消息缓冲、临界区。pthread、`std::thread`、`std::mutex`、`std::condition_variable` 和 sleep 都以任务运行。 |
| 时间 | `esp_timer`（任务和 ISR 两种派发）、`esp_timer_get_time`、`xTaskGetTickCount` 跟随真实时间。 |
| 内存 | 内部 RAM（`app_main` 开始时约 300 KB 可用）和 2 MB PSRAM 是两个独立的堆，容量与设备相同；`malloc`、`new`、任务栈和 DMA 缓冲都像在板子上一样从中分配；`heap_caps_*`、`CONFIG_SPIRAM_USE_MALLOC / CAPS_ALLOC / MEMMAP`、`esp_ptr_external_ram` 都可用。在板子上内部 RAM 不够的固件，在这里也会在差不多的地方不够。 |
| 屏幕 | `Display::*` 就是 HAL 自己的代码；panel 和 panel IO 句柄驱动一个 ST7789 模型（窗口、MADCTL、反色、睡眠、字节序），带 SPI 总线的耗时（按时钟算，每个窗口另加约 60 us 的命令和中断：80 MHz 时整屏约 7.4 ms）和 `on_color_trans_done`。GPIO38 上的 LEDC 就是背光。 |
| 按键、按键灯、HOME | `Keyboard::*` 把页面的按键交给 HAL 自己的去抖和四键上限；按键灯显示扫描会输出的亮度和总亮度。`System::GetHomeButton()` 和 GPIO0 就是 HOME。 |
| 音频 | `Audio::*`，以及经 I2C 访问的 ES8156 编解码器寄存器（启动耗时、音量）；TX 通道的 DMA（描述符 × 帧数）由页面的音频时钟按 44.1 kHz 取走，所以 `i2s_channel_write` 会像在板子上一样阻塞；有 `on_sent` / `on_send_q_ovf`。麦克风（`Mic::*`，PDM RX）在页面被允许时使用电脑的麦克风；GPIO43 归最后配置它的一方。 |
| 存储 | `Storage::*` 挂载 `/sdcard`（页面说没有卡时 `?sd=0` 就没有）；C 和 C++ 的文件操作都可用；`esp_vfs_fat_info`；NVS 随页面保存。 |
| USB | `USB::*`：MIDI 与电脑的 MIDI 端口互通（Web MIDI，在被允许时），系统 SysEx 命令，应用通道（也通向外层页面）；USB 音频和 HID 没有主机。 |
| 其他 | GPIO、GPTimer、`IR::Send` 用的 RMT（会解码显示）、ESP-IDF 日志、`esp_restart`（模拟器重新启动固件）、`esp_reset_reason`、`esp_random`、`esp_app_get_description`、任务看门狗的调用。 |

其他的都不提供，用到时编译失败并说明原因：其他 ESP-IDF 驱动和组件（Wi-Fi、蓝牙、UART、ADC、SPI 设备……）、寄存器访问（`soc/*_reg.h`）、屏幕或编解码器自己的初始化（归 HAL 负责）、Xtensa 和 ROM 内部、`esp_private/*`。所以能编译成网页版的固件就是遵循 HAL 的；编译不过的，会得到它越过 HAL 的那些调用的清单。

仍然存在的差别：浏览器的一个线程运行所有任务，在每次调用 FreeRTOS、ESP-IDF 或 HAL 时（以及两个核的任务之间）切换，所以一直循环、从不调用它们的任务会让其他一切停下（半秒后日志会点名）；`thread_local` 被所有任务共用；文件名区分大小写，打开着的文件也能改名；浏览器的时钟比板子粗（0.1 到 1 ms）；声音在 DMA 取走后约 60 ms 才到扬声器。

HAL 对网页目标的测试：`tests/web`（内核和各个模型，在电脑上运行）、`tests/web/browser`（无头 Chrome 里的页面）、`tests/web_tool_test.py`（构建工具）。

## 主机测试

[`tests/`](tests) 在电脑上检查 HAL，不需要板子和 ESP-IDF：纯计算部分（按键去抖和按键数上限、LED 帧、背光曲线，以及 Net 的：Launcher 保存网络的格式（用 Launcher 自己的代码生成的数据检验）、请求检查、尝试网络的顺序；在线服务的判断逻辑和 Launcher 的重启票据；固件文件夹的名字，以及在每一步之前都会断电的假卡上搬走旧文件夹）、驱动源码在故意出错的假 ESP-IDF 驱动下运行（让每条出错路径都走一遍），以及网页目标的内核和各个模型（`tests/web`）。CI 在每次推送和每个 PR 上运行它们。

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
- **[203 Launcher](https://github.com/203-Systems/203-launcher)**（MIT，源自 [bmorcelli/Launcher](https://github.com/bmorcelli/Launcher)）：系统菜单运行它的绘制、文字和动效代码，授权见 [licenses/203-launcher-LICENSE.txt](licenses/203-launcher-LICENSE.txt)。
- **HarmonyOS Sans**（华为终端有限公司）：系统菜单的中文、日文和越南文用由它生成的点阵（取自 203 Launcher），授权见 [licenses/HarmonyOS-Sans-LICENSE.txt](licenses/HarmonyOS-Sans-LICENSE.txt)。
