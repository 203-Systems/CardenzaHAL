# 为 Cardenza 开发固件

[English](developing.md)

这块板子上的 ESP32-S3 有哪些容易忽略的地方。这些都是在 CardenzaSynth 里踩过的坑，下面每个数字都是在板上实测的。HAL 的 README 讲它的接口；这里讲芯片和它周围的工具链。

## 内存

- **两种内存。** 内部 RAM（约 300 KB 堆）快，而且是 SD 卡的 DMA、中断和写闪存期间运行的代码唯一能用的内存。PSRAM（2 MB，四线，和闪存共用总线）慢：适合每一步、每一屏或按需读取的数据（Pattern、快照、采样、列表），不适合每个音频采样都要碰、中断或 DMA 要用的东西。SD 卡读进 PSRAM 时，驱动会退化成一次一个扇区。
- **别让 malloc 自己用 PSRAM**，除非你就是要这样：[`examples/psram_heap`](../examples/psram_heap) 把 PSRAM 映射好，以单独的能力加进堆，驱动和 C 库都留在内部 RAM，你的代码在需要的地方明确申请 PSRAM。
- **用 `MALLOC_CAP_INTERNAL` 量内部 RAM**：`heap_caps_get_free_size(MALLOC_CAP_8BIT)` 把 PSRAM 也算进去了。
- **`new` 失败会让板子重启。** ESP-IDF 默认不带异常编译，libstdc++ 的 `new (std::nothrow)` 内部调用普通 new，所以也会中止。把它们替换掉（示例里有）：nothrow new 返回空指针，内部 RAM 满了两者都转到 PSRAM。大块内存一律用 nothrow new，失败时在屏幕上说出来。用的时候才申请，不用就归还；不要大的静态缓冲；音频的每一块里不申请内存。
- **开了 PSRAM 后的原子操作。** ESP-IDF 会给所有代码加上 `-mdisable-hardware-atomics`：原子操作变成库函数调用，而 `std::atomic_flag::test_and_set` 变成普通的读和写，根本不是原子的（拿它做的自旋锁让板子卡死过）。改用 `std::atomic<bool>::exchange`。64 位原子操作是库函数，背后是两个核共用、关中断的一把锁：实时任务用的原子量保持 32 位。

## 热路径代码

- **`std::memcpy` 是一次 ROM 调用**，哪怕只拷一个字（ESP-IDF 用 `-fno-builtin-memcpy` 和 `-memset` 编译）。循环里用 `__builtin_memcpy`，配合 `__builtin_assume_aligned` 就是一条读指令（240 x 135 一帧的哈希从 3.6 ms 降到 0.4 ms）。
- **FPU 没有除法**：一次浮点除法是约 80 个周期的 ROM 调用。先算一次倒数再乘；每个采样里不做除法。
- **每个采样、每个声部调用一次函数，比声音本身还贵。** 按块渲染。
- **闪存里的代码经过 32 KB 指令缓存运行**（`CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB`），两个核共用。最热的代码用链接脚本片段（`.lf`）放进 IRAM，只放代码，只放每个采样都要跑的。满是 `1.0` 字面量的 C 代码要加 `-fsingle-precision-constant`，否则每个都是软件双精度。
- **闪存用 QIO、120 MHz**（[`board/sdkconfig.defaults`](../board/sdkconfig.defaults)）：DIO 下闪存里的代码要多花约 40%。

## 写闪存

写内部闪存期间，两个核的缓存都会停（一次 NVS 写约 45 ms）：闪存里的代码和不在 IRAM 的中断都跟着停（按键灯扫描停在某一行上，那一行就亮了）。数据放 SD 卡。如果一定要用 NVS，用自己的命名空间，绝不擦除整个分区：那里存着 203 Launcher 的已装软件登记。

## SD 卡上的文件

- **不要通过没有缓冲的 `FILE` 读**：newlib 会一个字节一个字节地读（一个 2 KB 的块花了 33 ms，还占住一个核）。对文件描述符用 `read()`，或者给 `FILE` 设缓冲。
- FAT 不让改名或删除打开着的文件，而且不分大小写（`Song` 和 `SONG` 是同一个文件）。
- 读一次卡约 2 ms，不管读多少：按几 KB 的块读，放在比界面低的任务里读，绝不在音频任务里读。

## 屏幕与时钟节拍

`Display::Present` 一次发 8 行，每发一条都要等：FreeRTOS 保持 1000 Hz（100 Hz 时整屏要 170 ms），只发变了的矩形（整屏是 17 ms 的 SPI）。

## USB

启动了 USB 的固件（`USB::Init`）会从 USB-Serial-JTAG 接过 PHY：它就没有串口了。用 [`tools/cardenza_usb.py`](../tools/cardenza_usb.py) 的 `console` 读它的控制台，用 `command download`（ROM 下载模式）或 `command launcher` 之后再刷。

## 卡死或崩溃时

- 调试版本加上 [`board/sdkconfig.debug`](../board/sdkconfig.debug)：任务卡住时板子会带着调用栈重启。
- 每次开机先打印版本号、Git 提交和 ELF 的哈希（`esp_app_get_elf_sha256`，见 [`cmake/firmware_info.cmake`](../cmake/firmware_info.cmake)），并保存给过别人的每个版本的 ELF：`xtensa-esp32s3-elf-addr2line -pfiaC -e <固件>.elf <地址>`。
- 把日志放在崩溃带不走的地方：RTC 慢速内存在 panic 和软件重启后还在（断电不行）。CardenzaSynth 的 `Devices/Cardenza/DeviceLog.cpp` 就保存这样一个环形缓冲，再复制到 SD 卡。

## 在 203 Launcher 下运行

Cardenza 平时运行 203 Launcher，由它安装和启动各个固件。被它启动的固件：

- 装在 Launcher 划出的分区里：从不改分区表、otadata 和 Launcher 自己的分区。在电脑上用 [`tools/uploader`](../tools/uploader/README.zh-CN.md) 把编好的固件交给 Launcher 安装（`serial_flasher.py`），再登记名称、UUID 和版本（`register_app.py`）。只有没装 Launcher 的板子才直接刷（即使那样，[`board/partitions.csv`](../board/partitions.csv) 也保留 Launcher 的区域，[`tools/cardenza_flash.py`](../tools/cardenza_flash.py) 拒绝抹掉 Launcher）；
- 数据放在 SD 卡上自己的文件夹里，不碰 Launcher 的 NVS 记录（见上）；
- 运行在 Launcher 的 bootloader 下，闪存是 QIO：按 QIO 构建；
- 在 app 描述里带上版本号：`PROJECT_VER`（`cardenza_read_version`）就是 Launcher 在它下面显示的版本。列表里的名字不是项目名，而是登记或安装时用的名字；直接刷进去的固件显示分区名；
- 用一次普通重启回到 Launcher（`esp_restart()`；从电脑上用 USB 的 `command launcher` 也行）；
- 用 [`.github/actions/launcher-publish`](../.github/actions/launcher-publish/action.yml) 发布到 Launcher hub：推送到 main 是 Nightly，`vX.Y.Z` 标签（等于它的版本号）是正式版。

完整规则（登记、元数据、安装）见 Launcher 自己的文档。
