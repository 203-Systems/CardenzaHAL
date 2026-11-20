# 在 CardenzaHAL 上做固件（摘要）

[English](FIRMWARE-PROJECT.md)

这是 [`docs/FIRMWARE-PROJECT.md`](https://github.com/203-Systems/CardenzaHAL/blob/main/docs/FIRMWARE-PROJECT.md) 的中文摘要。英文文件是唯一的规范（社区在 <https://cardenza.203.io/community/develop.md> 提供同一个文件，`203c rules` 也打印它）；两者不一致时以英文为准。

## 步骤

1. 装 203-CLI（社区命令行 `203c`，**必须**，下面每一步都用它；公开仓库 [203-Systems/203-CLI](https://github.com/203-Systems/203-CLI)，里面还有每台 203 设备的开发指引和硬件资料，鸣片的在 `products/cardenza`）。macOS、Linux、Windows 都一样（需要 Node.js 18+）：
   ```sh
   npm install -g 203c
   203c skill install     # 把 Agent Skill 放到 AI 读取的目录
   ```
   没装 203c，基于 HAL 的固件编译不了：`idf.py build`、`tools/cardenza.py build` 和网页目标都会停下并给出安装命令（CI 里不检查，发布那一步会自己装）。只克隆 203-CLI 仓库不算安装。
2. 建工程：`203c new "固件名" --summary "它做什么"`：生成一个 git 仓库，HAL 作为子模块放在 `components/CardenzaHAL`，从 [`examples/template`](https://github.com/203-Systems/CardenzaHAL/tree/main/examples/template) 复制并按固件名改好 `project()`、`203.json` 和发布工作流；直接能编译。改别人的固件：`203c fork <代码> [目录]`（克隆源码、建 Fork、写好 `203.json` 的 `forked_from`）。
3. 检查环境：`python3 components/CardenzaHAL/tools/cardenza.py doctor` 列出这台电脑还缺什么（ESP-IDF **5.3.5**、Emscripten、`203c`、登录），每项给出安装命令。登录每台电脑只要一次：`203c login` 打开浏览器，在社区网站上批准（远程时加 --no-browser，用链接和验证码）。
4. 写固件：只用 HAL 的公开头文件（硬件和对应头文件见 [README 的硬件一览](https://github.com/203-Systems/CardenzaHAL/blob/main/README.zh-CN.md#硬件一览)）；HAL 已有的驱动不要自己再写；只用 ESP-IDF；不引入传染性许可（copyleft）的代码；先调用 `CardenzaHAL::Init()`，缺硬件时报错但不要 abort 或重启。
   - **HOME 归系统**：按一下 HOME（侧键）弹出系统菜单（`system/menu.h`：继续、返回启动器、重新启动、关机、下载模式），是从任何固件回到 Launcher 的路。HOME 不要另作他用，给用户的固件也不要关掉菜单（`SystemMenu::Enable(false)`、`CONFIG_CARDENZA_HAL_SYSTEM_MENU`）。菜单打开期间固件的画面不发送、按键读作松开：不能在看不见时继续的事情先暂停（`SystemMenu::SetListener`），离开前要保存的在 `SystemMenu::SetLeaveHook` 里保存。
   - **按键照键帽上印的用**（[`keyboard/legends.h`](https://github.com/203-Systems/CardenzaHAL/blob/main/keyboard/legends.h)；键帽图在 [README](https://github.com/203-Systems/CardenzaHAL/blob/main/README.zh-CN.md#键盘与按键灯)）。方向键是四个黄色三角：`;` 上、`.` 下、`,` 左、`/` 右；文字输入时是 fn + 这几个键，不打字的画面（列表、菜单、游戏）直接用这几个键，203 Launcher 和系统菜单就是这样。ok 确认，esc（或 del）返回；文字输入时每个键打出印着的字符（按住 shift 是黄色的那个；del 删除）。不要自己另编一套：方向键不用 HJKL、IJKL 或 WASD，确认和返回也不换别的键。左边一列从上到下是 esc、tab、fn、ctrl（fn 是第三个键，29；左下角是 ctrl）；侧键 HOME 归系统菜单。
5. 编译（首选 HAL 的编译工具：先检查，shell 没激活时自己导出 ESP-IDF，再 `idf.py build`）：
   ```sh
   python3 components/CardenzaHAL/tools/cardenza.py build --web                # build/<工程名>.bin 和 build-web/site
   python3 components/CardenzaHAL/tools/cardenza_web.py serve build-web/site   # http://localhost:8000/
   ```
6. 在鸣片上试：装了 203 Launcher 的板子**不能** `idf.py flash`（会把 Launcher 刷掉；ESP-IDF 每次编译完都会这样提示）。通过 Launcher 安装并看日志：
   ```sh
   203c flash --bin build/<工程名>.bin                       # USB 线，不用登录；同一个固件会替换掉已装的那份
   203c log --until "<固件启动后打印的一行>" --seconds 30     # 读日志；出现这一行退出 0，崩溃或没出现退出 1
   ```
   Windows 用网站（Chrome 或 Edge）的「烧录本地固件」；日志用 `pip install hidapi` 后 `python components/CardenzaHAL/tools/cardenza_usb.py console 60`。
7. 第一个版本手动发布（只能通过 203c：手动或 CI）：提高 `version.txt`，在 `CHANGELOG.md` 写 `## <版本>`，然后
   ```sh
   203c publish --dry-run
   203c publish
   ```
   第一次发布后 203c 把固件代码写回 `203.json`：提交它。
8. 之后用 CI 发布（标准方式，203 Systems 的官方固件也一样）：模板里的 `.github/workflows/firmware.yml`（HAL 的 `community-publish` action）。仓库上了 GitHub 后运行 `203c link`：把这个仓库关联为固件的受信任发布者（也可以在固件页的「自动发布」里关联），并在固件页打开 beta、dev 渠道。推送到 `main` 发 dev，打 `vX.Y.Z` 标签发 stable，`vX.Y.Z-beta.N` 发 beta。受信任发布：作业用 GitHub 的 OIDC 令牌登录（`permissions: id-token: write`），不存任何密钥；CI 只发布版本，不改固件页。仓库搬家后 `203c link --move`。可选：自托管 runner 接一台鸣片，每次构建都装上去并用 `203c log --until` 检查能启动（模板里注释掉的 `on-device` 作业）。
9. 用户反馈和魔改版：每次改已发布的固件之前先运行 `203c sync`，读 `.203/community.md`：固件页、每个版本的更新说明、用户的需求（投票、讨论）和评论（回复、点赞）、合并请求和魔改版，上次之后的新内容有标记。`203c project diff <魔改版代码>` 看某个魔改版改了什么，`203c project diff 1.0.0 1.1.0` 比较两个版本；`203c comments add …` 回复用户。

## 必须有的文件

`203.json`（`format`、`name` 必填；`code` 首次发布前是 `null`；有 `version.txt` 时可不写 `version`；不能放任何密钥）、`CMakeLists.txt`（先列 HAL 的 `board/sdkconfig.defaults`）、`sdkconfig.defaults`、保留 Launcher 分区的分区表（同 HAL 的 `board/partitions.csv`，固件从 0x170000 开始）、`version.txt`、`LICENSE`、`README.md`、`CHANGELOG.md`、`.gitignore`（`build/`、`build-*/`、`.203/`、`sdkconfig`、`managed_components/`、`.env`、密钥文件）。

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
