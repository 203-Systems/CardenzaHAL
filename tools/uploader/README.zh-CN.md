# 上传和登记 Cardenza App

[English](README.md)

这些电脑端工具通过已经安装的 **203 Launcher** 上传 ESP32-S3 应用 BIN，
再登记应用 UUID、名称和版本。以下命令在 CardenzaHAL 根目录运行。
工具不编译固件，也不向在线商店发布应用。

## 准备环境

Windows、macOS、Linux 均可，使用 Python 3.9 或更新版本：

```sh
python -m venv .venv
# macOS / Linux
source .venv/bin/activate
# Windows PowerShell：.venv\Scripts\Activate.ps1
python -m pip install -r tools/uploader/requirements.txt
python -m serial.tools.list_ports -v
```

如有需要，把 `python` 换成 `python3`。把下文 `COM8` 换成实际端口，
macOS 通常是 `/dev/cu.usbmodem...`，Linux 通常是 `/dev/ttyACM0`。
关闭串口监视器和其他占用端口的程序。

设备必须已有可用的 Launcher，支持 `flash firmware`、`apps list` 和
`apps register`。应用必须针对 **Cardenza** 构建，使用它所需的共享分区布局。
优先选择 `build/<project>.bin` 应用文件，不要选择 bootloader 或分区表 BIN。
ESP32-S3 芯片检查不能证明固件引脚与 Cardenza 匹配。

## 1. 检查并上传

先离线检查镜像，无需连接设备或安装依赖：

```sh
python tools/uploader/serial_flasher.py --check -f ../MyApp/build/my_app.bin
```

让设备返回 Launcher 主界面，然后上传：

```sh
python tools/uploader/serial_flasher.py --no-reset -p COM8 -f ../MyApp/build/my_app.bin -n MyApp
```

脚本查询 `Launcher ...`，发送 `flash firmware MyApp <size>`，确认
`READY <size>` 后每次发送 2048 字节，逐块核对 `ACK <sent>/<size>`。
成功应答是 `OK flashed, rebooting`，设备随即重启进入新 App。
目标位置由 Launcher 的现有分配策略选择，不需要指定 Flash 地址。
`-n` 只是安装名称提示，允许 1–64 个 ASCII 字母、数字、点、下划线或连字符，
不会登记应用身份。

省略 `--no-reset` 时，脚本先用 esptool 复位，仅适用于 USB-Serial-JTAG
串口可用的情况。App 启用 HAL 复合 USB 后，原串口会消失：通过 App 的界面
或 HAL 系统命令 `0x40` 返回 Launcher（HID 报告 `0xCB`，或 MIDI SysEx
`F0 23 40 F7`），再重新列举端口。单纯复位可能恢复运行当前 App，因此推荐
先手动返回 Launcher。脚本不会自动发送 HID/MIDI 命令。

检查包含段边界、XOR 校验、镜像附带的 SHA-256（如有）和 ESP32-S3 芯片 ID。
也支持从 Flash 地址零开始、分区表位于 `0x8000`、仅包含一个有效应用的合并
BIN；只提取并发送应用部分。存在多个有效应用时会拒绝，请改用应用 BIN。
适用于未签名开发镜像，不支持签名或加密分发。设备仍会自行验证镜像。

## 2. 返回 Launcher，登记刚安装的应用

上传后设备已进入 App，需要再次返回 Launcher。登记工具不刷写、不复位。

```sh
python tools/uploader/register_app.py -p COM8 --list
python -c "import uuid; print(uuid.uuid4())"
```

在你的 App 工程中新建 `app.json`，把 UUID 换成刚生成的值：

```json
{
  "id": "3fbb1fe8-2dc4-40d6-955d-8a16d626ece3",
  "target": "cardenza",
  "name": "My App",
  "description": "Music and creative tools",
  "version": "0.1.0",
  "build": 1
}
```

同一 App 的后续构建保持 UUID 不变，不同 App 不要共用示例 UUID。
从 `--list` 输出确认实际分区 `label`，将下文 `app0` 替换为该标签：

```sh
python tools/uploader/register_app.py -p COM8 --label app0 --metadata ../MyApp/app.json
python tools/uploader/register_app.py -p COM8 --list
```

预期先看到 `OK registered`，随后列表中该条目为 `Registered`，UUID、名称、
版本和镜像 SHA-256 符合预期。Launcher 对设备中当前镜像计算哈希，将登记写入
NVS。一定要先核对分区标签：登记针对该分区当前的有效镜像。后续上传是一次
独立安装，需重新检查列表并明确登记当前构建。

| 字段 | 约束 |
| --- | --- |
| `id` | 必填，含连字符的小写 UUID |
| `name` | 必填，不能全为空白，最多 64 个 UTF-8 字节 |
| `description` | 可选，最多 160 个 UTF-8 字节 |
| `version` | 可选，最多 32 个 UTF-8 字节 |
| `build` | 可选整数，0–4294967295，默认 0 |
| `target` | 可选，提供时只能是 `cardenza` |
| `source` | 可选，`developer`（设备默认值）或 `manual` |

文本不能含 ASCII 控制字符，紧凑 JSON 总量最多 1024 个 UTF-8 字节。
Launcher 会拒绝与其他分区重复的 UUID，以及直接替换已有身份的操作。
同一 App 请保留现有 UUID；不同 App 请安装并登记在独立分区。

## 参数和故障处理

完整参数见各脚本的 `--help`。两者均支持 `-p/--port`、`-b/--baud`
（默认 115200）和 `--boot-timeout`（上传默认 15 秒，登记默认 10 秒）。
上传工具另有 `--launcher`，用于通过临时空闲分区明确更新 Launcher 本身；
只能提供 Cardenza Launcher 应用 BIN，并遵循下方上游更新说明。

| 现象 | 处理 |
| --- | --- |
| 缺 pyserial / esptool | 在运行脚本的同一 Python 环境安装 `tools/uploader/requirements.txt`。`--no-reset` 不需要 esptool。 |
| App 启动后串口消失 | 从 App 界面或 HAL 系统命令 `0x40` 返回 Launcher，再列举端口。 |
| Launcher 超时 / `ERR busy` | 回到主界面，关闭串口监视器，核对端口，用 `--no-reset` 重试。 |
| BIN 无效 / 合并镜像有多个应用 | 使用构建目录里的未签名 Cardenza 应用 BIN。 |
| ACK 超时 / 传输中断 | 停止工具，返回 Launcher 检查状态后再重试。脚本不会盲目重发二进制块。 |
| 无空闲空间 / 镜像被拒绝 | 按设备错误检查 Launcher 的已安装应用和剩余空间。 |
| UUID 重复 / 不能替换身份 | 核对 `--list`、分区和已有 UUID。 |

工具不执行备份。安装期间保持供电。离线检查只能验证镜像结构和协议行为，
不能证明硬件工作正常或安装具备断电安全性。本工具副本提供离线测试；
实际上传、重启及 NVS 持久化仍需实机验证。

## 来源和维护

基于 MIT 许可的 [203-launcher 0b0a187](https://github.com/203-Systems/203-launcher/tree/0b0a187)
中的 `tools/serial_flasher.py` 和 `tools/register_app.py` 完善，
保留许可于 [LICENSE.launcher](LICENSE.launcher)。
协议来源：[应用登记与系统更新](https://github.com/203-Systems/203-launcher/blob/main/docs/REGISTERED_SOFTWARE.md)、
[串口实现](https://github.com/203-Systems/203-launcher/blob/main/src/serial_console.cpp)。
上游命令变更时需核对协议并运行：

```sh
python -m unittest discover -s tools/uploader/tests -v
```
