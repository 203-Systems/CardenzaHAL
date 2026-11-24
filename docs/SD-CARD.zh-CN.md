# SD 卡的目录结构

[English](SD-CARD.md)

鸣片上的每个固件都和 203 Launcher、和别的固件共用一张 SD 卡（用户 2026-11-23 的决定）。卡的根目录下不放
任何文件：Launcher 一个文件夹，每个固件各一个文件夹，用户的截图一个文件夹（用户 2026-11-24）。

```
/sdcard/                      卡（FAT，由 CardenzaHAL::Init 挂载：storage/storage.h）
  Launcher/                   203 Launcher 的：它下载的固件、它的设置文件、备份、字库
  Apps/
    <AppName>/                一个固件的（storage/app_dir.h：Storage::AppDir()）
      Settings/               它的设置文件
      Logs/                   它的日志
      ...                     其余由固件自己安排（工程、音色、采样、存档、缓存）
    CardenzaSynth/            比如 Cardenza Synth 的
  screenshot/                 用户的截图，来自所有固件和 Launcher（HOME + 空格）
    2026-11-24_153012_CardenzaSynth.png
    shot-0001_Launcher.png    （截图时板子的时钟还没设好）
    .upload-queue             还要传到社区账号的（一个日志）
```

## 固件要遵守的

- **所有文件都放在自己的文件夹** `/sdcard/Apps/<AppName>/` 里：`Storage::AppDir()`、`Storage::AppPath("Settings/app.cfg")`。
  往一个文件夹里写之前先用 `Storage::EnsureAppDir("Settings")` 建好它（`/sdcard/Apps/<AppName>` 也一起建）。
  不往卡的根目录、`/sdcard/Launcher/` 或别的固件的文件夹里写；用户自己选了别的固件的文件（一个采样、一个工程）
  时可以读，不能写。
- **`Settings/` 和 `Logs/`**：用户（以及 `203cli board files` 这类工具）在这里找固件的设置和日志。其余的随意命名。
- **板子的设置不是文件**：音量、屏幕亮度、按键灯和语言由 HAL 存在 Flash 里，每个固件都一样
  （`system/settings.h`，[SETTINGS.md](SETTINGS.md)）。不要在卡上另存一份。
- **文件夹的名字**（`Storage::AppName()`）：设置了 `CONFIG_CARDENZA_HAL_APP_NAME`（menuconfig：CardenzaHAL）就用它，
  否则用应用描述里的工程名（CMake 的 `project()`，`203cli new` 会设好），再改成安全的 FAT 文件夹名：ASCII 字母、
  数字、`-`、`_` 和 `.`，其他字符变成 `_`（连着的几个变成一个），两端不留 `.`、`_`、`-`，最多 31 个字符，Windows
  保留的名字（`CON`、`NUL`、`COM1`……）后面加一个 `_`；什么都不剩时是 `App`。名字定下就别再改：以后改名就要把
  用户的文件搬过去（见下）。
- **没有卡是正常的**：照常运行（`Storage::IsMounted()` 为 false）；不要等卡、不要格式化、也不要因此重启。
- **用户随时可能拔电。** 写文件时先写到一个临时名字，再改名到位（FAT 不能改名覆盖已有文件：先删掉旧的；只剩
  临时文件时读临时文件）；模板里的 `WriteFile` / `ReadFile` 就是这样做的。
- 文件名：FAT 长文件名，固件自己生成的名字只用 ASCII（ESP-IDF 的 FAT 只编译了一个代码页），文件夹之间用 `/`，
  不用 `\ : * ? " < > |`。

模板（`examples/template`，`203cli new` 复制的就是它）全都照做了：它在 `Settings/app.cfg` 里数自己启动了几次，
每次启动往 `Logs/log.txt` 写一行。

## 搬走旧文件

以前把文件放在别处的固件（卡根目录下自己的文件夹，比如 Cardenza Synth 到 2026-11 为止用的 `/sdcard/CardenzaSynth/`），
在启动时、读任何文件之前搬一次：

```cpp
CardenzaHAL::Init();
CardenzaHAL::Storage::MigrateAppDir("/sdcard/MyFirmware");  // 然后再从 AppDir() 读 Settings/...
```

它做的事（`storage/app_dir_logic.h`，在每一步之前都会断电的假卡上测试过）：

| 卡上 | 结果 |
| --- | --- |
| 没有旧文件夹 | 什么都不做（一次 `stat`）：只要还可能有旧卡，就一直留着这个调用 |
| 只有旧文件夹 | 改一次名，变成 `/sdcard/Apps/<AppName>`（先建 `/sdcard/Apps`）：不复制任何内容，不管里面有多少东西都只要几毫秒 |
| 两个文件夹都在 | 合并：新文件夹里没有的，改名移进去；两边都有的文件夹，照同样的办法合并；两边都有的文件留在旧文件夹，绝不覆盖、绝不删除；最后变空的文件夹删掉 |
| 新文件夹的位置上是一个文件，或者卡拒绝了某一步 | 停在那里（已经搬过去的就在新地方），下次启动再试 |

- **任何时刻断电都不丢东西。** 每一步都只是一次改名、建一个文件夹，或者删一个空文件夹，所以每个文件不在旧地方
  就在新地方；下次启动重新走同样的步骤，把它做完。（FAT 改名时先写新名字、再清旧名字：恰好在这两步之间断电，
  两个名字会指向同样的文件，下次启动不会动它们；电脑上的磁盘检查可以把它整理好。）
- 搬了或者留下了东西时**写一行日志**（标签 `CardenzaStorage`）：`moved /sdcard/MyFirmware to /sdcard/Apps/MyFirmware`，
  或者 `merged ...: 3 moved, 2 left in the old folder (already there: Settings/app.cfg, ...)`。留下的东西一直在旧文件夹里，
  直到用户删掉（或者新文件夹里不再有同名的，下次启动就移进去）。
- 它需要已经挂载的卡，并且要独占卡：在别的代码用文件之前调用。

### 退回旧版本

搬完以后，用户可能装回旧版本的固件。旧版本还是去它的旧文件夹找：发现文件夹没了（或者只剩合并时留下的），就像用
一张新卡那样启动（默认设置，看不到用户的文件），并为它要保存的东西重新建起旧文件夹。它不会碰 `/sdcard/Apps/` 里的
任何东西。用户再装回新版本时，合并会把旧版本存在那里、而新文件夹里没有的东西搬过去；两边都有的（比如它的设置文件，
或者它改写过的某个工作文件）留在旧文件夹：想要那一份就手动拷过去。搬家的那个版本，在发布说明里写清楚这一点。

## 截图的文件夹

`/sdcard/screenshot/` 放所有固件和 Launcher 的截图（[SCREENSHOTS.zh-CN.md](SCREENSHOTS.zh-CN.md)）：
`<日期>_<时间>_<固件>.png`（UTC）或 `shot-NNNN_<固件>.png`，每张先写成临时的 `.<名字>.tmp` 再改名；还有 `.upload-queue`，
记录还要上传的截图（`Q`、`U`、`S` 行）。只有 HAL 和 Launcher 往这里写，固件自己的代码不写（要截图就调用 `Screenshot::Take()`）。
这个文件夹属于用户：可以在电脑上随便删；还没上传就被删掉的截图会被跳过。

## Launcher 的文件夹

`/sdcard/Launcher/` 是 203 Launcher 的（它下载的固件、它的设置文件、分区备份、字库）。固件不读也不写那里；Launcher
自己把它原来放在卡根目录的文件搬到了那里（203-launcher）。

## 在电脑上

鸣片通过 USB 连着、运行着基于 CardenzaHAL 的固件时，`203cli board files` 可以读写它的卡（`docs/usb-dev.md`）：
`203cli board files ls Apps/<AppName>`、`203cli board files get Apps/<AppName>/Logs/log.txt`。插在电脑读卡器里的卡也是同样的文件夹。

网页版（`tools/cardenza_web.py`）也有一张挂在 `/sdcard` 的卡，存在浏览器里：同样的路径都能用。

### 作为 U 盘借给电脑

编进了 `CONFIG_CARDENZA_HAL_USB_DRIVE` 的固件可以把卡借给电脑（`USB::Drive::Share`，
[`usb/drive.h`](https://github.com/203-Systems/CardenzaHAL/blob/main/usb/drive.h)）：电脑看到整张卡，包括每个固件的文件夹和
Launcher 的文件夹，而在卡回来之前固件什么也看不到。借出期间 `Storage::LentToComputer()` 为真，`/sdcard` 下的所有文件操作都会失败：
共享时不要在卡上留着打开的文件（监听函数收到 `State::Starting` 时就是关闭的时机），卡回来（`State::Off`）后重新读一遍设置，因为
用户可能改过。把那时看到的卡当作刚从电脑读卡器里拿出来的：文件可能新增、改动或者不见了，拷到一半拔线可能留下被截断的文件。像模板那样
用断电安全的方式写文件，就不怕这些。
