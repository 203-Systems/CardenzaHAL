# 截图

[English](SCREENSHOTS.md)

CardenzaHAL 上的每个固件都用同一种方式截图，203 Launcher 也一样（用户 2026-11-24：“给HAL加入一个截图功能。会把内容保存到
SD卡里面的固定位置/screenshot，并且可以在社区的个人主页里面访问。”以及“按住Home键加右下角空格键截屏 … 确保所有固件是统一的”）。
固件不用写代码：`CardenzaHAL::Init` 会启动它（`CONFIG_CARDENZA_HAL_SCREENSHOT`，默认打开；`screenshot/screenshot.h`）。

## 怎么截

- **HOME + 空格**（侧键和右下角那个印着空心方框的键）：先按哪个都行，同时按也行，一个松开后很快再按另一个也行（200 ms 以内，
  `Logic::kComboWindowMs`：按键偏硬、晚了一点点也算）。按住 HOME 时每按一次空格就再截一张。
- **通过 USB**：`203cli board screenshot`（存到卡上，同时把 PNG 写到电脑上；`--no-save` 只取回不存），也就是开发通道的
  `SCREENSHOT`（`0x12`，[usb-dev.md](usb-dev.md)），系统通道走的每条路都能用（HID、SysEx、WebUSB、串口）；串口上也可以直接输入
  `screenshot`（回一行 JSON，写着文件名）。
- **固件自己调用**：`CardenzaHAL::Screenshot::Take()`（立刻返回；结果由 `SetListener` 告诉你）。

### HOME 和系统菜单

有了截图以后，系统菜单改为在**松开** HOME 时打开：短按（30 ms 到 1 s）且没有截图，松开后 200 ms 打开（这 200 ms 里按了空格就变成截图）。
按得更久什么也不打开。固件启动时、或菜单关闭时 HOME 已经按着的，要先松开才算数，和以前一样。

| 按键 | 结果 |
| --- | --- |
| 按下 HOME，1 秒内松开，别的键没按 | 松开 200 ms 后打开菜单 |
| 按住 HOME，再按空格 | 立刻截图；松开 HOME 不打开菜单 |
| 按住空格（多久都行），再按 HOME | 截图 |
| 两个同时按下 | 截图 |
| 点一下 HOME，200 ms 内按空格 | 截图，不打开菜单 |
| 点一下空格，200 ms 内按 HOME | 截图 |
| 点一下 HOME，200 ms 以后才按空格 | 打开菜单（空格归固件） |
| 按住 HOME 1 秒以上，别的键没按 | 什么也不发生 |

这些情况都在 `tests/screenshot_test.cpp` 里对手势（`Logic::HomeGesture`）逐一测试。

### 空格和固件

- **先按 HOME 再按空格**（HOME 按着，或松开不到 200 ms）：这次空格归系统，固件完全看不到（`updateKeyList` 把它藏到松开为止）。
- **先按空格再按 HOME**：空格已经交给固件了。固件照常立刻收到空格，因为音乐固件的 PLAY 不能等；窗口内跟上 HOME 照样截图，
  只是固件已经响应过这次空格。在 Cardenza Synth 里空格就是 PLAY：先按空格会先开始（或停止）播放，再由 HOME 截图；先按 HOME
  则只截图，不碰播放。
- **可选的延迟**：空格不要求实时的固件可以调用 `Screenshot::SetSpaceWait(true)`。之后每次空格都晚 200 ms 才交给固件；变成截图的那次
  永远不会交给固件；比窗口短的一下点按，会在窗口过后变成一次 30 ms 的点按交给固件。
- **HOME**：系统菜单开着时 HOME 归系统，固件看不到（`System::GetHomeButton` 一直是 false）。关掉了菜单的固件
  （`SystemMenu::Enable(false)`）和以前一样能收到 HOME，而 HOME + 空格仍然是截图（所有固件截图方式一致）。

## 截到的是什么

就是固件显示的那 240 x 135 个像素，原样，不包含系统菜单和截图效果：

| 固件怎么画 | 截图截到的 |
| --- | --- |
| `Display::Present`、`PresentChanged`、frame presenter、`DoubleBuffer` | 它最后送出的那一帧（从按下 HOME 起保留；USB 和 `Take` 取它下一次送出的帧，250 ms 内没有就取最后一帧） |
| 自己用屏幕的句柄画（`esp_lcd_panel_draw_bitmap`：LVGL、自己的代码） | 它画的内容，由 HAL 在它画的时候顺手复制一份（PSRAM 里 65 KB，第一次画时分配；没有 PSRAM 就截不到） |
| 自己直接发 SPI（`esp_lcd_panel_io_tx_color`） | 截不到：卡片上会写“无法截取这个画面”，USB 回答 `no framebuffer` |

屏幕没有读回的线（MISO 没接），HAL 读不回屏幕上的内容，只能保留发出去的。

## 文件

- 板子的时钟已经设好时是 `/sdcard/screenshot/2026-11-24_153012_CardenzaSynth.png`（UTC；同一秒里第二张是 `…153012-2_…`），
  否则是 `shot-0001_CardenzaSynth.png`（文件夹里已有的最大编号加一）。时钟在这次开机里板子联网过之后，由在线服务按社区回答里的
  `"now"` 设好，或者由固件自己设。固件部分是它的工程名（`[A-Za-z0-9-]`，最多 32 个字符）；Launcher 的是 `Launcher`。
- 原样像素的 PNG（`screenshot/png.h`）：RGB565 扩展成每通道 8 位（r8 = r5 << 3 | r5 >> 2，所以还能读回 565 的值）；
  画面不超过 256 种颜色时（大多数界面）是索引色 PNG，1–10 KB；否则是 8 位 RGB（最坏 110 KB）。
- 先写成 `.<名字>.tmp`，同步后再改名：断电不会留下半个文件（残留的 `.tmp` 在下一次截图时删掉）。没有卡：什么也不写，
  卡片上会写“没有 SD 卡”。
- `/sdcard/screenshot/` 是卡根目录下 `/Launcher/`、`/Apps/` 之外的第三个文件夹（[SD-CARD.zh-CN.md](SD-CARD.zh-CN.md)）：
  属于用户，所有固件和 Launcher 共用。

## 画面效果

所有固件和 Launcher 都一样（`screenshot/card.h`，和系统菜单一样用 Launcher 自己的绘图代码画）：先是一下白色闪光，
通过 4 x 4 有序抖动淡出（180 ms）；然后底部出现一张白色卡片，上面是缩小到四分之一的截图和结果（“截图已保存”和文件名，
或者“没有 SD 卡”/“写入 SD 卡失败”/“内存不足”加“未保存”），用板子的语言，出现和消失也走同样的抖动；一共 1.6 秒。
只有淡入淡出；每个像素要么是固件的、要么是闪光的白、要么是卡片的，从不混合。

这期间固件照常运行：声音、按键、画面都不停（卡片画在它最新的画面上面，只发送变化的部分）。卡片消失后，固件的画面原样回来，
它的下一帧整帧发送。自己用屏幕句柄画的固件，在 `Screenshot::IsShowing()` 期间应暂停绘制，和系统菜单打开时一样。

## 上传

- **默认打开**，板子设置 `screenshot_upload`（HOME > 设置 > 截图自动上传，下面一行小字写着“HOME + 空格键截图”；
  `203cli board settings set screenshot_upload off`；[SETTINGS.md](SETTINGS.md)）。关着时截的图只留在卡上。
- 每张存好的截图记进 `/sdcard/screenshot/.upload-queue` 这个日志（`Q <名字>` 待上传，`U <名字>` 已上传，`S <名字>` 永久跳过；
  最早的先传；全部传完就删掉这个文件）。Launcher 读写同一个文件：在固件里截的图，之后也可能由 Launcher 上传。
- 在固件里由在线服务（[net/presence.h](../net/presence.h)）上传：只在板子绑定了账号并且通过它联网时（Launcher 绑定过、
  存过 Wi-Fi、“接收固件”开着），每次查询收件箱之后，一次最多 4 张，在它优先级 1 的任务里，固件的活动
  （`Presence::SetActivity`）或音频来不及时不传。`POST <api>/device/screenshots`，带设备令牌，PNG 作为请求体（最大 256 KiB），
  `X-Screenshot-Name`、`X-Taken-At`（时钟设好时）、固件名、版本和镜像摘要。
- 社区的回答：存好了或已经有了 → `U`；不是 PNG 或太大被拒 → `S`；账号满了（`quota_full`）或社区还没有这个接口 → 这次开机不再传；
  其他情况 → 下次再传。设备令牌不会发到别处，也不会写进日志。
- **默认私密**：本人在社区的个人主页（截图标签）里看到，自己决定哪些公开。
- **没有 Wi-Fi**：社区网站可以通过 USB 读卡，把账号里还没有的截图传上去。

## 代价

平时几乎为零：每次 `updateKeyList` 看一眼空格（几条指令），以及和系统菜单共用的 HOME 任务。截一张图：一份 65 KB 的画面拷贝和
编码器的 32 KB（有 PSRAM 就用 PSRAM；没有 PSRAM 时编码器只有在内部 RAM 还剩 48 KB 以上时才用内部 RAM，并且直接读固件的帧缓冲），
在一个优先级 1、6 KB 栈的任务里完成，最后一张截完 10 秒后任务退出；编码大约 20–80 ms。画面效果：4 KB 缩略图和 11 KB 卡片
（有 PSRAM 就用 PSRAM），持续 1.6 秒。Flash：约 18 KB。上传：每张截图让在线服务多发一个请求。

## 固件里怎么用

```cpp
#include "screenshot/screenshot.h"

CardenzaHAL::Screenshot::SetListener([](const CardenzaHAL::Screenshot::Shot& shot, void*) {
    // shot.status：Saved、NoCard、WriteFailed、NoFrame、NoMemory；shot.name 在 /sdcard/screenshot/ 里
}, nullptr);
CardenzaHAL::Screenshot::Take();                    // 和 HOME + 空格一样
CardenzaHAL::Screenshot::Take({.save = true, .effect = false});
CardenzaHAL::Screenshot::SetSpaceWait(true);        // 只在空格不要求实时的固件里用
```

给开发固件的 AI：`203cli board screenshot --no-save --out /tmp/screen.png --json` 一条命令取回屏幕
（`board screen` 取的是原始帧，没有效果，也不存盘）。
