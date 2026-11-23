# 鸣片上又快又少撕裂的屏幕

[English](DISPLAY.md)

怎样用 CardenzaHAL 的帧提交器（[`display/presenter.h`](../display/presenter.h)、[`display/display.h`](../display/display.h)）把 240 × 135 的屏幕用好：它替你做了什么、循环怎么定节拍、帧缓冲放哪、每帧花多少、这块屏在撕裂上能做什么不能做什么。它就是 CardenzaSynth 的屏幕刷新方式，做成了通用的，再加上 203 Launcher 在这块屏上测出来的结论。

## 最短的用法

```cpp
#include "cardenzaHAL.h"

namespace Display = CardenzaHAL::Display;

alignas(4) static uint16_t frame[Display::WIDTH * Display::HEIGHT];  // 或者 Display::AllocateFramebuffer()

extern "C" void app_main()
{
    CardenzaHAL::Init();
    Display::FramePacer pacer;              // 每 10 ms 一拍，每两拍画一帧：50 fps
    for (;;)
    {
        Update();                           // 按键、声音、状态：每拍都做（10 ms 延迟）
        if (pacer.DrawTick())
        {
            Draw(frame);                    // 整幅画面，按时间算出来
            Display::PresentChanged(frame); // 只把变了的发给屏幕
        }
        pacer.Wait();
    }
}
```

每帧按状态和时间画整幅画面（动画是时间的函数，不需要第二块帧缓冲）：`PresentChanged` 自己找出和上一帧不同的地方，只发那些。

## `PresentChanged` 做了什么

1. **找出变了的地方。** 每行按 30 像素一段算哈希（一次一个字：帧在内部 RAM 时板子上一帧 0.4 ms），和上一帧比。每条 8 行的带子给一个矩形，覆盖变了的行和变了的段的横向范围；上下相邻、横向范围相同的带子合成一个。哈希表 4.3 KB（有 PSRAM 就放 PSRAM）。
2. **每帧整条重发一带**，每次换一条：17 帧（50 fps 时 0.34 秒）整屏刷新一遍，每帧多发 3.8 KB，哈希漏掉的或屏幕丢了的都不会一直留在屏上。
3. **按屏幕的扫描顺序发。** 矩形切成每块最多 2,160 像素的窗口，从左往右发（[`display/scan_order.h`](../display/scan_order.h)，见下面「撕裂」）：高的矩形按 16 列一条发，矮而宽的一块发完。
4. **经过两块内部 RAM 的 DMA 块。** 帧缓冲放哪都行：HAL 把每个窗口复制进一块（屏幕的 DMA 读不了 PSRAM），前一块还在线上时把下一个窗口复制进另一块。等 DMA 的任务在一块发完时立刻醒来：HAL 在 DMA 中断里请求切换任务，ESP-IDF 的 SPI 屏幕驱动自己不做这件事。
5. **80 MHz**（`CONFIG_CARDENZA_HAL_DISPLAY_SPI_MHZ`），203 Launcher 在每台鸣片上给这块屏用的时钟。
6. **不碍系统菜单的事**：菜单打开时什么都不发（`FrameStats::held`），关掉后的第一帧整屏发。开发通道的截屏（`203cli board screen`）取的就是你提交的帧缓冲。

第一帧和发送失败后的那一帧都整屏发。`Present(framebuffer)` 用同样的方式发整帧；`Present(framebuffer, regions, count)` 发你自己知道变了的矩形（两种方式选一种，不要混用：别的 `Present` 不会更新 `PresentChanged` 的哈希表）。

## 循环的节拍

`FramePacer` 让循环按固定的拍子走，每几拍画一帧（`PacerOptions`：`tickMs = 10`，`ticksPerFrame = 2`）。按键和声音控制保持一拍的延迟，屏幕 50 fps。某一拍超时了就不睡（后面几拍会追上）；连续三拍超时，循环无论如何睡一个 RTOS tick，让同一个核上比它优先级低的任务（写卡的任务、空闲任务和它的看门狗）能跑：CardenzaSynth 的界面曾经把一个核占到 93%，它们一点时间都没有，看门狗把板子重启了。`LateTicks()` 和 `Yields()` 分别计数。

拍子的长度是整数个 RTOS tick。板子的默认配置里 FreeRTOS 是 1000 Hz，任何毫秒数都行；ESP-IDF 默认的 100 Hz 下最短一拍是 10 ms。屏幕本身不再依赖 tick 频率（2026-11-23 以前每条带子都要等到下一个 tick：1000 Hz 时整屏 17 ms，100 Hz 时 170 ms）。

## 帧缓冲放在哪

屏幕的 DMA 从不直接读你的帧缓冲，所以两种 RAM 都可以：

| | PSRAM | 内部 RAM |
| --- | --- | --- |
| 占用的内部 RAM | 不占 | 约 300 KB 里的 64.8 KB |
| 找出变化 | 每帧经缓存读 64.8 KB：更慢（看 `FrameStats::findUs`） | 每帧 0.4 ms（CardenzaSynth） |
| 复制要发的部分 | 约 40 MB/s（估计值） | 约 400 MB/s |

`AllocateFramebuffer(FrameMemory::Any)` 返回清零、32 字节对齐的一帧：板子的堆里有 PSRAM 就放 PSRAM，否则放内部 RAM（也可以指定 `Psram` 或 `Internal`）。静态的 `alignas(4)` 数组在内部 RAM，除非你把它放进 PSRAM（`EXT_RAM_BSS_ATTR` 配合 `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`）。

HAL 自己用的内存：一块 4.3 KB 的 DMA 块（静态）；第二块在第一帧时从内部堆申请（`PresentOptions::pipeline`，默认开；申请不到就只用一块）；4.3 KB 的哈希表（有 PSRAM 就放 PSRAM）。

## 每帧花了多少

- `LastFrame()`：上一帧的 `bytes`、`regions`、`windows`、`findUs`（算哈希）、`sendUs`（复制和发送，直到最后一个窗口到了屏上）、`presentUs`，以及它是不是整屏（`whole`）、是不是被系统菜单挡住（`held`）、有没有失败。
- `Totals()` / `ResetTotals()`：帧数、整屏帧数、被挡住的帧数、失败次数、字节数、提交时间的总和与最大值。
- `SetFrameCallback(callback, context)`：每次提交帧缓冲之后调用，在提交的那个任务上、显示锁之外（回调里可以调用显示的函数）。适合偶尔写一行日志或在屏幕上显示一个计量条；要短。

模板（[`examples/template`](../examples/template)）每分钟写一行日志：`screen: 3000 frames, 1523 us a frame (max 26851), 3832 bytes a frame, 15 late ticks`（这是网页模拟器里的数）。

## 上一帧在发的时候画下一帧

`DoubleBuffer` 有两块帧缓冲（129.6 KB，有 PSRAM 就放 PSRAM）和 HAL 的一个任务：它提交一块时你往另一块画下一帧，这样一帧花的是画和发两者中较长的那个，而不是两者之和。`Back()` 是现在要画的那块（里面是上上帧：要画整幅画面），`Present()` 把它交出去就返回。代价是一个任务（4 KB 栈）；它对撕裂没有任何帮助：屏幕显示的是它自己的显存。

## 撕裂

这块屏（ST7789）的撕裂效应（TE）信号没有接到芯片上，HAL 也不读回它的扫描行（板上没有 MISO 线），所以 HAL 无法知道屏幕刷新到哪儿了。屏幕每秒约 60 次把显存扫到玻璃上，按屏幕的列一列接一列，从 x = 0 开始（在这个横屏方向上，屏幕的原生行就是画面的列；203 Launcher 实测过）。扫描经过时正在写的内容，在那一次刷新（16.7 ms）里可能一半是旧的一半是新的。

帧提交器能做的：

- **从左往右，顺着扫描写。** 从上到下整行地写会和扫描交叉：凡是盖满屏幕的变化都会在某处碰上扫描，沿一条斜线裂开。从左往右按列条写是顺着扫描：碰上的那次刷新只在一条竖直的直线上分开，而且碰上得更少。
- **快。** 80 MHz 时整屏约 7.6 ms，远小于一次刷新；40 MHz 时和扫描差不多一样长，两者并排走，撕成一级一级的台阶（203 Launcher 2026-11-04 的评审）。
- **只发变了的，一口气发完。** 小的变化不到一毫秒就到屏上了。

没有 TE 信号就无法解决的：扫描正好经过某个变化的那几列时写进去的变化，在那一次刷新里会在扫描线处分开。概率约为（它的宽度（列数）× 48 us + 写它的时间）/ 16.7 ms：24 像素的小图约 17%，横贯整个宽度的条约 70%（每移动一帧都是），和写的顺序无关。所以动效要按这块屏来设计：

- 每帧小步移动。分开处显示的是两帧之间的差别：每帧滑 2-3 像素的条分开 2-3 像素，很难看出来；一下跳一整行就看得出。
- 换页在一帧里完成（确定全变了时调用 `RequestWholeFrame()`）。约 30% 的换页会碰上扫描，只影响一次刷新。分好几帧的滑动或擦除，每一帧都是一次整屏的变化。
- 能窄就窄：概率随变化的宽度增大。
- 内存里的双缓冲没用（见上），画得更快也没用：分开是屏幕什么时候扫描决定的，不是你的帧决定的。

以上数字来自 [`tests/display_bench.cpp`](../tests/display_bench.cpp) 里的模型（见下），用的是 ST7789 的默认时序；这块屏真实的行时间还没有测过。

## 调整

`SetPresentOptions(PresentOptions)`；下一帧整屏发：

- `segmentPixels`（30）：哈希段的宽度，能整除 240 的偶数，每行最多 32 段（8 … 240）。16 让矩形和扫描的列条对齐，哈希表 8.1 KB。
- `refreshBands`（1）：每帧整条重发的 8 行带子数（0 到 17）。0 时没变化的帧什么都不发。
- `pipeline`（true）：第二块 DMA 块。

`CONFIG_CARDENZA_HAL_DISPLAY_SPI_MHZ`（80）是总线时钟（芯片能给出 80、40、26.7 MHz……）。只有在某块板子屏幕异常、要排除时钟问题时才降低它。

## 数字

`tests/display_bench.cpp` 在典型画面上跑 HAL 自己的变化查找和扫描顺序，并按模型估算板子上的 SPI 时间（用 ESP-IDF 给 S3 的 SPI 事务耗时；模型能复现以前实测的 17 ms 和 170 ms）。编译主机测试（`cmake -S tests -B build-tests && cmake --build build-tests`），运行 `build-tests/CardenzaDisplayBench`。每帧，帧缓冲在 PSRAM，含每帧重发的那条带子：

| 画面 | 字节 | 以前（40 MHz，每条带子等 tick） | 现在（80 MHz，扫描顺序，两块） | 一块 | 撕裂概率 以前 / 现在 |
| --- | --- | --- | --- | --- | --- |
| 什么都不动 | 3.8 KB | 1.5 ms | 0.54 ms | 0.54 ms | 0 / 0 |
| 一个 60 × 14 的数字在变 | 5.0 KB | 2.4 ms | 0.73 ms | 0.76 ms | 21% / 19% |
| 24 × 24 的小图每帧移 2 像素 | 6.3 KB | 4.0 ms | 0.86 ms | 0.92 ms | 25% / 17% |
| 240 × 18 的条每帧滑 3 像素 | 6.8 KB | 3.0 ms | 0.94 ms | 1.04 ms | 74% / 71% |
| 整屏滚动 | 64.8 KB | 17.5 ms | 7.6 ms | 9.1 ms | 100% / 30% |

这些是模型算出来的：板子上的真实数字用 `FrameStats` 测。
