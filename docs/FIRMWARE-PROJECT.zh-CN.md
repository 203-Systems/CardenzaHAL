# 在 CardenzaHAL 上做固件（摘要）

[English](FIRMWARE-PROJECT.md)

这是 [`docs/FIRMWARE-PROJECT.md`](https://github.com/203-Systems/CardenzaHAL/blob/main/docs/FIRMWARE-PROJECT.md) 的中文摘要。英文文件是唯一的规范（社区在 <https://cardenza.203.io/community/develop.md> 提供同一个文件，`203cli rules` 也打印它）；两者不一致时以英文为准。

## 步骤

鸣片的每个固件都必须基于 CardenzaHAL（git 子模块，`203cli new` 会加上）：按键和键灯、屏幕、音频、麦克风、SD 卡、USB、HOME 和系统菜单、网络，都只通过它来用。不用 Arduino、不用别的框架、不自己写驱动。

1. 装 203-CLI（社区命令行 `203cli`，**必须**，下面每一步都用它；公开仓库 [203-Systems/203-CLI](https://github.com/203-Systems/203-CLI)，里面还有每台 203 设备的开发指引和硬件资料，鸣片的在 `products/cardenza`）。macOS、Linux、Windows 都一样（需要 Node.js 18+）：
   ```sh
   npm install -g 203cli
   203cli skill install   # 把 Agent Skill 放到 AI 读取的目录
   ```
   没装 203cli，基于 HAL 的固件编译不了：`idf.py build`、`tools/cardenza.py build` 和网页目标都会停下并给出安装命令（CI 里不检查，发布那一步会自己装）。只克隆 203-CLI 仓库不算安装。
   有更新时 203cli 和编译都会提示（每天最多查一次，查不到不出声，CI 里不查；`NO_UPDATE_NOTIFIER=1` 关掉）：「203cli X is out」就运行它给的 `npm install -g 203cli && 203cli skill install`；「CardenzaHAL in this project is N commits behind its main」就单独做一次更新：`git submodule update --remote components/CardenzaHAL`，编译并在板子上试过，再只提交子模块指针（`git add components/CardenzaHAL`）。`203cli updates` 立刻查。
   在 AI 的沙箱里（Claude Code 网页版、Codex 云端……）：npm 和 GitHub 一般默认能用，203 社区不在默认白名单里，只有用户能在环境的网络设置里放行 `cardenza.203.io` 和 `apps.203.io`（Claude Code 网页版：claude.ai/code 输入框下面的环境选择器 → Cloud → 环境的设置图标 → Network access 选 Limited，保持勾选 Allow package managers，在 Allowed domains 里加上这两个 → Save changes；仓库里的文件做不到）。`203cli doctor` 会检查这两个域名，被拦时 203cli 报 `network_blocked_sandbox` 并给出每一步（`203cli help sandbox`）；`network_blocked_cdn` 是网站的防火墙，不是沙箱。编译还可能要 Espressif 的域名（`*.espressif.com`）。
2. 建工程：`203cli new "固件名" --summary "它做什么"`：生成一个 git 仓库，HAL 作为子模块放在 `components/CardenzaHAL`，从 [`examples/template`](https://github.com/203-Systems/CardenzaHAL/tree/main/examples/template) 复制并按固件名改好 `project()`、`203.json` 和发布工作流；直接能编译。改别人的固件：`203cli fork <代码> [目录]`（克隆源码、建 Fork、写好 `203.json` 的 `forked_from`）。
3. 检查环境：`python3 components/CardenzaHAL/tools/cardenza.py doctor` 列出这台电脑还缺什么（ESP-IDF **5.3.5**、Emscripten、`203cli`、登录），每项给出安装命令。登录每台电脑只要一次：`203cli login` 打开浏览器，在社区网站上批准（远程时加 --no-browser，用链接和验证码）。
4. 写固件：只用 HAL 的公开头文件（硬件和对应头文件见 [README 的硬件一览](https://github.com/203-Systems/CardenzaHAL/blob/main/README.zh-CN.md#硬件一览)）；HAL 已有的驱动不要自己再写；只用 ESP-IDF；不引入传染性许可（copyleft）的代码；先调用 `CardenzaHAL::Init()`，缺硬件时报错但不要 abort 或重启。
   - **音量、亮度、按键灯和语言是用户的**：它们是板子的设置（`system/settings.h`，HOME > 设置，[docs/SETTINGS.md](https://github.com/203-Systems/CardenzaHAL/blob/main/docs/SETTINGS.md)），`CardenzaHAL::Init` 会用上。固件启动时不要自己设（`Audio::SetVolume`、`Display::SetBrightness`、`Keyboard::setLEDGlobalBrightness`：会盖过用户的选择），也不要自己另存一份；固件自己的电平（比如混音的总轨）照旧归固件，在板子的音量之前。用 `Settings::Get` 读，用 `Settings::Set` 做快捷键，录到的声音交给 `Mic::ApplyGain`。
   - **文件放在 SD 卡上固件自己的文件夹 `/sdcard/Apps/<AppName>/`**（每个固件共用的目录结构：[docs/SD-CARD.zh-CN.md](https://github.com/203-Systems/CardenzaHAL/blob/main/docs/SD-CARD.zh-CN.md)）：`Storage::AppDir()`、`Storage::AppPath("Settings/app.cfg")`、`Storage::EnsureAppDir("Settings")`（`storage/app_dir.h`）。`Settings/` 和 `Logs/` 是约定的子文件夹，其余随固件安排。不往卡的根目录、`/sdcard/Launcher/`（203 Launcher 的）或别的固件的文件夹里写。文件夹以 CMake 工程名（`project()`）命名，或者用 `CONFIG_CARDENZA_HAL_APP_NAME`：定下就别再改。以前把文件放在别处的固件，在启动时、读任何文件之前调用 `Storage::MigrateAppDir("/sdcard/<旧文件夹>")`（只改一次名；两边都有时合并，绝不覆盖；断电也安全）。模板已经这样做了。
   - **HOME 归系统**：按一下 HOME（侧键）弹出系统菜单（`system/menu.h`：继续、设置、返回启动器、重新启动、关机、本机信息、下载模式），是从任何固件回到 Launcher 的路。HOME 不要另作他用，给用户的固件也不要关掉菜单（`SystemMenu::Enable(false)`、`CONFIG_CARDENZA_HAL_SYSTEM_MENU`）。菜单打开期间固件的画面不发送、按键读作松开：不能在看不见时继续的事情先暂停（`SystemMenu::SetListener`），离开前要保存的在 `SystemMenu::SetLeaveHook` 里保存。
   - **按键照键帽上印的用**（[`keyboard/legends.h`](https://github.com/203-Systems/CardenzaHAL/blob/main/keyboard/legends.h)；键帽图在 [README](https://github.com/203-Systems/CardenzaHAL/blob/main/README.zh-CN.md#键盘与按键灯)）。方向键是四个黄色三角：`;` 上、`.` 下、`,` 左、`/` 右；文字输入时是 fn + 这几个键，不打字的画面（列表、菜单、游戏）直接用这几个键，203 Launcher 和系统菜单就是这样。ok 确认，esc（或 del）返回；文字输入时每个键打出印着的字符（按住 shift 是黄色的那个；del 删除）。不要自己另编一套：方向键不用 HJKL、IJKL 或 WASD，确认和返回也不换别的键。左边一列从上到下是 esc、tab、fn、ctrl（fn 是第三个键，29；左下角是 ctrl）；侧键 HOME 归系统菜单。
5. 编译（首选 HAL 的编译工具：先检查，shell 没激活时自己导出 ESP-IDF，再 `idf.py build`）：
   ```sh
   python3 components/CardenzaHAL/tools/cardenza.py build --web                # build/<工程名>.bin 和 build-web/site
   python3 components/CardenzaHAL/tools/cardenza_web.py serve build-web/site   # http://localhost:8000/
   ```
6. 在鸣片上试：装了 203 Launcher 的板子**不能** `idf.py flash`（会把 Launcher 刷掉；ESP-IDF 每次编译完都会这样提示）。通过 Launcher 安装并看日志：
   ```sh
   203cli flash --bin build/<工程名>.bin                     # USB 线，不用登录；同一个固件会替换掉已装的那份
   203cli log --until "<固件启动后打印的一行>" --seconds 30   # 读日志；出现这一行退出 0，崩溃或没出现退出 1
   ```
   Windows 用网站（Chrome 或 Edge）的「烧录本地固件」；日志用 `pip install hidapi` 后 `python components/CardenzaHAL/tools/cardenza_usb.py console 60`。
   `203cli board` 通过 USB 查询正在运行的固件，固件不用写代码：`info`（是什么、怎么编译的、Launcher、芯片、上次为什么重启）、`partitions`（板子上的分区表和占用）、`memory`（内存、PSRAM、每个任务的栈和 CPU）、`storage`、`status`、`i2c`、`screen`（截图 PNG）、`press`（从电脑按键，不碰板子也能测界面）、`coredump`（上次崩溃）、`files`（SD 卡上的 `ls`、`get`、`put`、`rm`、`mkdir`、`mv`）。要先启动 USB：在 `CardenzaHAL::Init()` 之后加一行 `CardenzaHAL::USB::Init();`（模板没有启动）。任务、CPU 占用和崩溃转储要用板子的调试配置（`board/sdkconfig.debug`）。协议：[docs/usb-dev.md](https://github.com/203-Systems/CardenzaHAL/blob/main/docs/usb-dev.md)。
7. 先把固件页准备好：看固件页决定装不装的是普通用户，不是开发者，所以固件页要像产品介绍，不是说明书（[写法](https://cardenza.203.io/community/guide/description)，`203cli help description`）。
   - **简介**是 `docs/description.md`（`publish.description_file`；`203cli new`、`203cli init` 会按模板写好，把每个 `<…>` 换成自己的内容）：一两句话说清它是什么、给谁用，`## 功能` 3–6 条，有要求时写 `## 需要`，`## 开始` 1–3 行，最后一行指向它的教程。目标 800 个字符以内（插图不算）；超过 1500 时 203cli 和网站会提醒，新写或改过的简介超过 3000 就保存不了。不写进简介：键位表和每个模式的按键（放教程）、怎么编译（放 README，README 是给开发者的，不是固件页）、版本变化（放发布说明）。
   - **图片**（简介上面的大图）：3–5 张不同的固件截图，第一张是用户最先看到的画面，然后每个主要功能一张。固件在 USB 线连着的鸣片上运行时（要启动 USB：`CardenzaHAL::USB::Init()`），`203cli board screen --out docs/screens/home.png` 存一张 PNG；写进 `203.json` 的 `publish.images`（或者 `203cli publish --image …`）。用自己的登录 `203cli publish` 时，图片不是 3–5 张不同的就在上传任何东西之前停下（`gallery_pictures`）；在网页上发布只是建议。CI 的凭证不能改图片，所以发布不会因为图片失败：203cli 给出警告（`gallery_short`），`community-publish` action 把它显示在运行页上，由固件的主人补一次（网站上固件的编辑页 › 图片，或者用自己的登录）。网页模拟器没有截图功能。
8. 第一个版本手动发布（只能通过 203cli：手动或 CI）：提高 `version.txt`，在 `CHANGELOG.md` 写 `## <版本>`，然后
   ```sh
   203cli publish --dry-run
   203cli publish
   ```
   第一次发布后 203cli 把固件代码写回 `203.json`：提交它。
9. 之后用 CI 发布（标准方式，203 Systems 的官方固件也一样）：模板里的 `.github/workflows/firmware.yml`（HAL 的 `community-publish` action）。仓库上了 GitHub 后运行 `203cli link`：把这个仓库关联为固件的受信任发布者（也可以在固件页的「自动发布」里关联），并在固件页打开 beta、dev 渠道。推送到 `main` 发 dev，打 `vX.Y.Z` 标签发 stable，`vX.Y.Z-beta.N` 发 beta。受信任发布：作业用 GitHub 的 OIDC 令牌登录（`permissions: id-token: write`），不存任何密钥；CI 只发布版本，不改固件页。仓库搬家后 `203cli link --move`。可选：自托管 runner 接一台鸣片，每次构建都装上去并用 `203cli log --until` 检查能启动（模板里注释掉的 `on-device` 作业）。
10. 用户反馈和魔改版：每次改已发布的固件之前先运行 `203cli sync`，读 `.203/community.md`：固件页、每个版本的更新说明、用户的需求（投票、讨论）和评论（回复、点赞）、合并请求和魔改版，上次之后的新内容有标记。`203cli project diff <魔改版代码>` 看某个魔改版改了什么，`203cli project diff 1.0.0 1.1.0` 比较两个版本；`203cli comments add …` 回复用户。

## 必须有的文件

`203.json`（`format`、`name` 必填；`code` 首次发布前是 `null`；有 `version.txt` 时可不写 `version`；不能放任何密钥）、`CMakeLists.txt`（先列 HAL 的 `board/sdkconfig.defaults`）、`sdkconfig.defaults`、保留 Launcher 分区的分区表（同 HAL 的 `board/partitions.csv`，固件从 0x170000 开始）、`version.txt`、`LICENSE`、`README.md`（给开发者：怎么编译、怎么试）、`docs/description.md`（固件页的简介）、`CHANGELOG.md`、`.gitignore`（`build/`、`build-*/`、`.203/`、`sdkconfig`、`managed_components/`、`.env`、密钥文件）。

## 社区发布前检查什么

- `image`：ESP32-S3 应用镜像，带 app 描述（工程名、版本、IDF）。不通过就打回。
- `layout`：镜像（按 64 KB 向上取整）放得进 Launcher 给应用的空间：Cardenza 从 0x170000 起 0x690000 字节。不通过就打回。
- `boot`：在 QEMU 里按 Launcher 的方式启动 20 秒；`app_main` 之前崩溃、看门狗复位、反复重启都会打回；缺硬件（屏幕、音频、SD 卡、USB、Wi-Fi）导致的错误只列出来，不算。
- `hal`：镜像或启动日志里有 HAL 的标记或 `CardenzaHAL` 日志标签；没有只是警告。
- `simulator`：网页版 15 秒内发出 `{"cardenza_sim":"ready"}`（HAL 的网页在 `app_main` 一运行就发）；不通过只是警告，没有「可试玩」标记。

被打回：`203cli versions check <版本代码>` 看报告；真问题就修好、用新的版本号发布；误判（只因模拟器没有硬件）就申诉。

## 版本与渠道

`stable`（默认，要有更新说明）、`beta`、`dev`（保留最新 30 个）。版本号 1–32 个 `0-9 A-Z a-z . _ + -`，同一固件内唯一，不能低于该渠道最新的版本；stable / beta 的版本号要和 BIN 里的版本一致，dev 可以写成 `<版本>-dev.<序号>`。CI 发布时渠道跟着 Git 引用走：推送到 `main` → dev（`<版本>-dev.<运行号>`），`vX.Y.Z` 标签（等于 `version.txt`）→ stable，`vX.Y.Z-beta.N` → beta。不在 GitHub Actions 上的 CI 才需要个人令牌（设置 › 个人令牌，`write`），放进 CI 的密钥 `COMMUNITY_TOKEN`。

## Fork

用 `203cli fork` 开始；`forked_from` 记录来源；事后补：`203cli firmware based-on <代码> <来源>`。保留原作的 LICENSE 和版权声明，在 README 里注明来源。Launcher 只有官方的 203 Launcher，只有管理员能发布。官方固件也按上面的标准方式发布（有主人的官方固件由主人的 CI 发布）；只有从旧 Launcher hub 镜像来、没有主人的官方固件不接受 203cli 的版本。

## 绝不能提交

令牌（`cct_`、`ccd_`、`cfu_`、`upl_`、`COMMUNITY_TOKEN` 的值）、`.env`、私钥和证书（`*.pem`、`*.key`、`id_rsa*` 等）、其他服务的令牌、Wi-Fi 密码、带密钥的 `sdkconfig`。203cli 拒绝含密钥的 `203.json`，服务器拒绝含密钥的源码包。泄露过的令牌要到 `/me` 撤销重建。
