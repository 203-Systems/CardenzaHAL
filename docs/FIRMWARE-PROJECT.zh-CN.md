# 在 CardenzaHAL 上做固件（摘要）

[English](FIRMWARE-PROJECT.md)

这是 [`docs/FIRMWARE-PROJECT.md`](https://github.com/203-Systems/CardenzaHAL/blob/main/docs/FIRMWARE-PROJECT.md) 的中文摘要。英文文件是唯一的规范（社区在 <https://cardenza.203.io/community/develop.md> 提供同一个文件，`203c rules` 也打印它）；两者不一致时以英文为准。

## 步骤

1. 装好工具：ESP-IDF **5.3.5**（`idf.py --version`）、Emscripten（`emsdk install latest`，网页版要用）、Node.js 18 以上，以及 `203c`：
   ```sh
   curl -fsSL https://cardenza.203.io/community/skill/install.sh | sh
   203c doctor
   203c login      # 打开浏览器：在社区网站上批准（远程时加 --no-browser，用链接和验证码）
   ```
2. 建工程：从 [`examples/template`](https://github.com/203-Systems/CardenzaHAL/tree/main/examples/template) 复制一份，把 HAL 加成子模块：
   ```sh
   git submodule add https://github.com/203-Systems/CardenzaHAL.git components/CardenzaHAL
   ```
   改别人的固件：`203c fork <代码> [目录]`（克隆源码、建 Fork、写好 `203.json` 的 `forked_from`）。
3. 写固件：只用 HAL 的公开头文件（`cardenzaHAL.h`、`keyboard/keyboard.h`、`display/display.h`、`audio/audio.h`、`mic/mic.h`、`storage/storage.h`、`system/system.h`、`ir/ir.h`、`usb/usb.h` 等）；HAL 已有的驱动不要自己再写；只用 ESP-IDF；不引入传染性许可（copyleft）的代码；先调用 `CardenzaHAL::Init()`，缺硬件时报错但不要 abort 或重启。
4. 编译：
   ```sh
   idf.py set-target esp32s3 && idf.py build                                   # build/<工程名>.bin
   python3 components/CardenzaHAL/tools/cardenza_web.py build .                # build-web/site（含 sim.json）
   python3 components/CardenzaHAL/tools/cardenza_web.py serve build-web/site   # http://localhost:8000/
   ```
   装了 203 Launcher 的板子**不能** `idf.py flash`（会把 Launcher 刷掉）：从 Launcher 安装，或用 `tools/cardenza_flash.py --app-into <分区名>`。
5. 第一个版本手动发布：提高 `version.txt`，在 `CHANGELOG.md` 写 `## <版本>`，然后
   ```sh
   203c publish --dry-run
   203c publish
   ```
   第一次发布后 203c 把固件代码写回 `203.json`：提交它。
6. 之后用 CI 发布（标准方式，203 Systems 的官方固件也一样）：用模板里的 `.github/workflows/firmware.yml`（HAL 的 `community-publish` action），在固件页的「自动发布」里关联这个仓库和工作流文件（或 `203c firmware publishers add`），并在固件页打开 beta、dev 渠道。推送到 `main` 发 dev，打 `vX.Y.Z` 标签发 stable，`vX.Y.Z-beta.N` 发 beta。受信任发布：作业用 GitHub 的 OIDC 令牌登录（`permissions: id-token: write`），不存任何密钥；CI 只发布版本，不改固件页。

## 必须有的文件

`203.json`（`format`、`name` 必填；`code` 首次发布前是 `null`；有 `version.txt` 时可不写 `version`；不能放任何密钥）、`CMakeLists.txt`（先列 HAL 的 `board/sdkconfig.defaults`）、`sdkconfig.defaults`、保留 Launcher 分区的分区表（同 HAL 的 `board/partitions.csv`，固件从 0x170000 开始）、`version.txt`、`LICENSE`、`README.md`、`CHANGELOG.md`、`.gitignore`（`build/`、`build-*/`、`sdkconfig`、`managed_components/`、`.env`、密钥文件）。

## 社区发布前检查什么

- `image`：ESP32-S3 应用镜像，带 app 描述（工程名、版本、IDF）。不通过就打回。
- `layout`：镜像（按 64 KB 向上取整）放得进 Launcher 给应用的空间：Cardenza 从 0x170000 起 0x690000 字节。不通过就打回。
- `boot`：在 QEMU 里按 Launcher 的方式启动 20 秒；`app_main` 之前崩溃、看门狗复位、反复重启都会打回；缺硬件（屏幕、音频、SD 卡、USB、Wi-Fi）导致的错误只列出来，不算。
- `hal`：镜像或启动日志里有 HAL 的标记或 `CardenzaHAL` 日志标签；没有只是警告。
- `simulator`：网页版 15 秒内发出 `{"cardenza_sim":"ready"}`（HAL 的网页在 `app_main` 一运行就发）；不通过只是警告，没有「可试玩」标记。

被打回：`203c versions check <版本代码>` 看报告；真问题就修好、用新的版本号发布；误判（只因模拟器没有硬件）就申诉。

## 版本与渠道

`stable`（默认，要有更新说明）、`beta`、`dev`（保留最新 30 个）。版本号 1–32 个 `0-9 A-Z a-z . _ + -`，同一固件内唯一，不能低于该渠道最新的版本；stable / beta 的版本号要和 BIN 里的版本一致，dev 可以写成 `<版本>-dev.<序号>`。CI 发布时渠道跟着 Git 引用走：推送到 `main` → dev（`<版本>-dev.<运行号>`），`vX.Y.Z` 标签（等于 `version.txt`）→ stable，`vX.Y.Z-beta.N` → beta。不在 GitHub Actions 上的 CI 才需要个人令牌（设置 › 个人令牌，`write`），放进 CI 的密钥 `COMMUNITY_TOKEN`。

## Fork

用 `203c fork` 开始；`forked_from` 记录来源；事后补：`203c firmware based-on <代码> <来源>`。保留原作的 LICENSE 和版权声明，在 README 里注明来源。Launcher 只有官方的 203 Launcher，只有管理员能发布。官方固件也按上面的标准方式发布（有主人的官方固件由主人的 CI 发布）；只有从旧 Launcher hub 镜像来、没有主人的官方固件不接受 203c 的版本。

## 绝不能提交

令牌（`cct_`、`ccd_`、`cfu_`、`upl_`、`COMMUNITY_TOKEN` 的值）、`.env`、私钥和证书（`*.pem`、`*.key`、`id_rsa*` 等）、其他服务的令牌、Wi-Fi 密码、带密钥的 `sdkconfig`。203c 拒绝含密钥的 `203.json`，服务器拒绝含密钥的源码包。泄露过的令牌要到 `/me` 撤销重建。
