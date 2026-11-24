# Making a firmware on CardenzaHAL

The rules for a firmware project on [CardenzaHAL](https://github.com/203-Systems/CardenzaHAL), the
hardware layer of the 203 Systems Cardenza, and for publishing it on the 203 firmware community: what
to install, which files a project has, how it uses the HAL, how it builds, what the community checks,
versions, forks, publishing and what never goes into the repository.

**The standard release is the same for every firmware, 203 Systems' own and everyone else's**: its versions go
to the 203 community on its channels (stable, beta, dev), from CI with the HAL's `community-publish` action and
trusted publishing ([Publish](#8-publish)). The community is where devices get firmware: the 203 Launcher
installs and updates by a firmware's code from it. Building from source and flashing your own board stay
possible; they are not how a firmware is released.

This file is the single source. It lives in CardenzaHAL
([`docs/FIRMWARE-PROJECT.md`](https://github.com/203-Systems/CardenzaHAL/blob/main/docs/FIRMWARE-PROJECT.md));
the community serves the same file at <https://cardenza.203.io/community/develop.md>, `203cli rules`
prints it, and the community's developer guide (<https://cardenza.203.io/community/develop>) starts
from it. It is written for people and for coding agents alike: imperative, with exact commands.

- The HAL's API: [README](https://github.com/203-Systems/CardenzaHAL/blob/main/README.md).
- The chip and the toolchain, learnt the hard way: [docs/developing.md](https://github.com/203-Systems/CardenzaHAL/blob/main/docs/developing.md).
- A project to start from: [examples/template](https://github.com/203-Systems/CardenzaHAL/tree/main/examples/template).
- **Required**: CardenzaHAL itself. Every firmware for the Cardenza is built on it (a git submodule, `203cli new` adds it) and reaches the hardware only through it: keys and their lights, the display, audio, the microphone, the SD card, USB, HOME and the system menu, the network. No Arduino core, no other framework, no drivers of your own ([Using the HAL](#3-using-the-hal)).
- **Required**: `203cli`, the community's CLI, with its Agent Skill and the development guide and hardware reference of every 203 device (the Cardenza's included): [203-CLI](https://github.com/203-Systems/203-CLI). Install it with `npm install -g 203cli && 203cli skill install`; cloning the repository does not install it, and a firmware on the HAL does not build without it.

## If you are an AI agent

"Make a metronome for the Cardenza" and the like: do these in order; stop and ask the user only where a step says so.

1. Read this file to the end once. Then read the HAL's [README](https://github.com/203-Systems/CardenzaHAL/blob/main/README.md): its [hardware table](https://github.com/203-Systems/CardenzaHAL/blob/main/README.md#the-hardware) says what the Cardenza has and which header drives it.
2. Install `203cli` if `203cli --version` fails: `npm install -g 203cli`, then `203cli skill install` ([Setup](#nodejs-and-203cli)). It is required, for every step below: a firmware's build stops without it (cloning 203-CLI is not installing it). In a sandboxed agent (Claude Code on the web, Codex cloud…) the community's hosts must be allowed first: `203cli doctor` checks them, and when 203cli says `network_blocked_sandbox`, ask the user to allow `cardenza.203.io` and `apps.203.io` in the environment's network settings ([Running in a sandboxed agent](#running-in-a-sandboxed-agent-claude-code-on-the-web-codex-cloud)); you cannot do it yourself.
3. Set up the project: a new one with `203cli new "<Name>" --summary "<what it does>"` ([Start a project](#start-a-project)); someone else's firmware with `203cli fork <CODE> [DIR]` ([Forks](#7-forks-and-provenance)).
4. `python3 components/CardenzaHAL/tools/cardenza.py doctor` in the project: it lists what this computer lacks (ESP-IDF 5.3.5, Emscripten, 203cli, the sign-in) with the command for each. Install what is missing ([Setup](#1-dependencies-and-setup)).
5. Sign in once: when doctor says "not signed in", run `203cli login`: it opens the user's browser on the community, they approve, and it comes back signed in (tell the user to approve 203cli in the browser). Not on the user's computer (SSH, CI, a remote agent): `203cli login --no-browser`, give the user the link and the code it prints, then `203cli login --wait`. Never ask for passwords or tokens.
6. Write the firmware following [the HAL rules](#3-using-the-hal): public headers only, nothing the HAL already does, no copyleft code.
7. Build with `python3 components/CardenzaHAL/tools/cardenza.py build --web` (the board and the browser; [Build](#4-build)); fix every error and warning of yours.
8. When the user has a Cardenza on USB: `203cli flash --bin build/<project>.bin`, then `203cli log --until "<a line your firmware prints when it is ready>" --seconds 30` ([Trying it on a board](#trying-it-on-a-board)). Never `idf.py flash`: it erases the 203 Launcher.
9. Raise `version.txt` and add a `## <version>` entry to `CHANGELOG.md` (the template starts at 0.1.0 with one).
   The page ([The page](#the-page-a-short-description-and-35-screenshots)): fill in `docs/description.md` from what the firmware does (short, for people deciding to install: `203cli help description`; ask the user what matters to them when you do not know), and with it running on the user's Cardenza take 3–5 different screenshots (`203cli board screen --out docs/screens/1.png`, …) into `203.json`'s `publish.images`. Never use pictures that are not of this firmware.
10. The first version: `203cli publish --dry-run`, fix what it reports, then `203cli publish` ([Publish](#8-publish)). Ask the user before the real publish, and before `--force`. Publishing goes through 203cli only (by hand or from CI).
11. When the checks send the version back, read the report ([Checks](#5-what-the-community-checks)); fix and publish the next version, or, only for a false positive, appeal.
12. Commit `203.json` (203cli writes the firmware's code into it). Never commit a secret ([Never commit](#9-never-commit)).
13. The standard release ([Publish from CI](#publish-from-ci-the-standard-release)): once the repository is on GitHub, `203cli link` makes it the firmware's trusted publisher (the template has the workflow). From then on a push to `main` is a dev version and a tag `vX.Y.Z` the release.
14. Before every change to a published firmware: `203cli sync`, then read `.203/community.md`: the page, every version's notes, what users asked for and said (requests with votes and their discussion, comments with replies), merge requests and forks, new ones marked. Work from it, answer with `203cli comments add …`, release the next version. `203cli project diff <FORK>` shows what a fork changed.
15. When 203cli or the build says an update is out ("203cli X is out", "CardenzaHAL in this project is N commits behind its main"): update 203cli at once as the line says; update the HAL as a change of its own, built and tried, its pointer committed alone, after the task at hand or when the user agrees ([Updates](#updates-203cli-and-the-hal)).

## 1. Dependencies and setup

| Tool | Version | For |
| --- | --- | --- |
| ESP-IDF | **5.3.5** | the device build |
| CardenzaHAL | upstream `main` (or a commit on it) | the hardware layer, as a git submodule |
| Emscripten (emsdk) | `latest` | the simulator version (web build) |
| CMake, ninja, Python 3 | CMake 3.20+ | the web build (ESP-IDF's own are fine) |
| Node.js | 18+ | `203cli` |
| `203cli` | current (**required**) | a new project, installing on a Cardenza and its log, signing in, `203.json`, publishing, users' feedback |
| git | any | the submodule, forks, source archives |

### ESP-IDF 5.3.5

The firmware builds with ESP-IDF 5.3.5, the version the official firmware and the HAL's board defaults
are checked with (the HAL needs 5.3 or later; take 5.3.5).

Linux (Debian, Ubuntu):

```sh
sudo apt-get install git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
mkdir -p ~/esp
git clone -b v5.3.5 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v5.3.5
~/esp/esp-idf-v5.3.5/install.sh esp32s3
. ~/esp/esp-idf-v5.3.5/export.sh          # in every shell that builds
idf.py --version                            # ESP-IDF v5.3.5
```

macOS: `xcode-select --install`, `brew install cmake ninja dfu-util python3`, then the same `git clone`,
`install.sh esp32s3` and `. export.sh`.

Windows: the ESP-IDF Tools Installer (<https://dl.espressif.com/dl/esp-idf/>), choosing **v5.3.5**; build
in the "ESP-IDF 5.3 PowerShell" (or CMD) it installs. In a clone, `install.ps1 esp32s3` once and
`export.ps1` in every shell.

You do not have to activate ESP-IDF yourself to build: the HAL's `tools/cardenza.py build` finds an ESP-IDF 5.3 in
`IDF_PATH`, `~/esp/esp-idf*` or `C:\Espressif\frameworks` and exports it for the build (an agent's shell starts
fresh every time).

### CardenzaHAL as a git submodule

The HAL is a git submodule at **`components/CardenzaHAL`**. ESP-IDF finds `components/` by itself, and the
folder's name is the component's name (`REQUIRES CardenzaHAL`).

```sh
git submodule add https://github.com/203-Systems/CardenzaHAL.git components/CardenzaHAL
git commit -m "Add CardenzaHAL"
```

- After cloning a project that has it: `git clone --recursive <url>`, or `git submodule update --init --recursive`.
- Update it: `git submodule update --remote components/CardenzaHAL`, build and test, then commit the new pointer (`git add components/CardenzaHAL`). 203cli and the build say when it is behind its `main` ([Updates](#updates-203cli-and-the-hal)).
- Keep it a clean copy of upstream: never edit, commit or push inside it, and never pin it to a commit that is not on upstream `main`. `python3 components/CardenzaHAL/tools/check_submodule.py --fetch` fails when it is not (run it before builds; it also makes pushes from the submodule impossible).
- Missing or wrong in the HAL? Open an issue or a pull request at <https://github.com/203-Systems/CardenzaHAL>; do not patch your copy.
- Elsewhere than `components/`? Put the folder in `EXTRA_COMPONENT_DIRS` **before** `include($ENV{IDF_PATH}/tools/cmake/project.cmake)`, and keep the folder named `CardenzaHAL` (the official CardenzaSynth keeps it at `Devices/Cardenza/CardenzaHAL`).

The HAL brings its own dependency, TinyUSB (`espressif/tinyusb ~0.21.0`, through ESP-IDF's component
manager: the first build downloads it into `managed_components/` and writes `dependencies.lock`; commit
the lock). Do not add `esp_tinyusb` or anything else that configures TinyUSB.

### Emscripten (the simulator version)

The same source also builds for the browser with the HAL's web target
([`tools/cardenza_web.py`](https://github.com/203-Systems/CardenzaHAL/blob/main/tools/cardenza_web.py));
ESP-IDF is not needed for it.

```sh
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
~/emsdk/emsdk install latest
~/emsdk/emsdk activate latest
source ~/emsdk/emsdk_env.sh               # or leave it: the tool looks for ~/emsdk by itself
```

The tool finds Emscripten as `emcmake` on `PATH`, else in `$EMSDK`, else in `~/emsdk`. It also needs
CMake 3.20 or later (`PATH`, or `CMAKE=<path>`) and ninja or make; ESP-IDF's are fine (after
`export.sh` they are on `PATH`). No version is pinned: the HAL's CI builds with `emsdk install latest`, and
the build records the one it used in `sim.json` (`toolchain.emscripten`). Exit code 4 means Emscripten or
CMake is missing; its message says how to install it.

### Node.js and 203cli

`203cli` is the community's command line (an Agent Skill with its CLI), from the public repository
[203-CLI](https://github.com/203-Systems/203-CLI), which also holds each 203 device's development guide and hardware
reference (`products/cardenza` is this one). It needs Node.js 18 or later. It is required: every build of a firmware
on the HAL checks for it (`idf.py build`, `tools/cardenza.py build` and the web target alike) and stops without it,
with the command to install it; CI builds skip that check, since their publish step installs 203cli.

On macOS, Linux and Windows alike:

```sh
npm install -g 203cli
203cli skill install
203cli doctor
203cli login
```

- `npm install -g 203cli` gives the `203cli` command (no dependencies, no install scripts); `203cli skill install` puts its Agent Skill where coding agents read it (`~/.claude/skills/203-community`, and `~/.codex/skills` when Codex is there). Run both again to update; 203cli says when a newer one is out ([Updates](#updates-203cli-and-the-hal)), and `203cli doctor` when the skill is older than the command.
- 203cli runs wherever Node does; it uses the USB cable (`flash`, `log`) on macOS and Linux, and on Windows the website does that in Chrome or Edge.
- `203cli login` opens the browser on the community's sign-in page: sign in there (GitHub) if it asks, approve "203cli on <your computer>", and the browser goes back to 203cli (OAuth's authorization code with PKCE and a redirect to 203cli on `127.0.0.1`). Where no browser can be opened, make a personal token on the site (设置 › 个人令牌, `/me/tokens`) and run `203cli login --with-token`: it asks for the token (not shown), checks it and saves it (a script pipes it in: `203cli login --with-token < token.txt`). An agent running remotely signs in with `203cli login --no-browser`: a link and a code to approve on the site. Every way saves a personal token, listed on the site under 设置 › 个人令牌, where you can delete it. You sign in once: it has no end.
- Everything the community's API does for you, 203cli does (`203cli --help`; `203cli api` for anything else): use 203cli, not raw HTTP.
- Every command except `login`, `logout`, `help`, `version`, `doctor` and `rules` needs that sign-in.
- 203cli keeps its token in your home folder, never in the project. GitHub Actions needs no token at all: trusted publishing ([Publish from CI](#publish-from-ci-the-standard-release)).

### Updates: 203cli and the HAL

203cli and every build say when one of the two is out of date: 203cli against its newest release on npm, the HAL
submodule against CardenzaHAL's `main` on GitHub, each asked at most once a day, silently when there is no answer
(offline, a sandbox's allowlist), never in CI.

- `203cli 3.6.0 is out (you have 3.5.1): npm install -g 203cli && 203cli skill install`: run both (the skill is the
  command's own copy), then go on.
- `CardenzaHAL in this project (components/CardenzaHAL) is 12 commits behind its main: …`: update it as a change of
  its own, built and tried before it is committed:

  ```sh
  git submodule update --remote components/CardenzaHAL
  python3 components/CardenzaHAL/tools/cardenza.py build --web    # then 203cli flash --bin … and 203cli log on a board
  git add components/CardenzaHAL
  git commit -m "CardenzaHAL: up to its main"
  ```

  What changed: the HAL's commits since the old pointer (`git -C components/CardenzaHAL log --oneline <old>..HEAD`,
  or <https://github.com/203-Systems/CardenzaHAL/commits/main>). An agent in the middle of a task finishes it first, or
  asks the user.
- `CardenzaHAL in this project (…) is not a clean copy of upstream main: …` (the HAL's own `check_submodule.py`): put
  it back as the line says; a change to the HAL goes upstream ([the submodule](#cardenzahal-as-a-git-submodule)).

Where they appear: on stderr when 203cli's commands of the work end (`sim`, `flash`, `log`, `board`, `new`, `fork`,
`clone`, `init`, `link`, `publish`, `rules`, `source`, `sync`); as a CMake warning in `idf.py build`,
`tools/cardenza.py build` and the web build (`cmake/require_203cli.cmake` runs `203cli updates --quiet`, with 203cli 3.5.1
or later); listed by `203cli doctor` and `tools/cardenza.py doctor`; `203cli updates` asks now. They never change a
command's answer or exit code. Off with `NO_UPDATE_NOTIFIER=1` (or `C203_NO_UPDATE_CHECK=1`).

### Running in a sandboxed agent (Claude Code on the web, Codex cloud…)

A coding agent in a cloud sandbox reaches only the hosts its environment allows, and only the user can change that,
per environment (no file in the repository can). Package registries and GitHub are on Claude Code's default list, so
`npm install -g 203cli` and `203cli skill install` work there; the 203 community is on no default list.

- **Allow `cardenza.203.io` and `apps.203.io`** (the community and its twin). Claude Code on the web: at
  <https://claude.ai/code>, the environment selector under the message box → Cloud → the environment's settings
  (gear) icon → Network access: **Limited**, keep **Allow package managers** on, add both to **Allowed domains** (one
  per line) → **Save changes** (or choose **Full**); a session already open follows within about a minute. A shared
  Team or Enterprise environment: an Owner changes it in claude.ai's admin settings › Cloud environments. See
  [Claude Code's cloud environments](https://code.claude.com/docs/en/cloud-environments.md#network-access). Codex
  cloud and other sandboxes keep their own allowlist per environment (Codex cloud: the environment's internet
  access): add the same two hosts there.
- Building may need Espressif's hosts as well (the first build fetches TinyUSB from Espressif's component registry):
  allow `*.espressif.com`, or install ESP-IDF and build once in the environment's setup script.
- `203cli doctor` checks both hosts. A blocked request is `network_blocked_sandbox` (the sandbox's allowlist, with these
  steps in its `hint`) or `network_blocked_cdn` (the site's firewall, not the sandbox: report its Ray ID); a host
  that does not resolve (`ENOTFOUND`) in a sandbox is usually the same allowlist. `203cli help sandbox` has it all.
- Until then, what needs no network still works: building, `203cli rules`, `203cli skill install`, and on a computer
  with the Cardenza on its USB cable `203cli flash --bin build/<project>.bin`, `203cli log` and `203cli board`.

## 2. The project: files and layout

```text
my-firmware/
├── 203.json                 the firmware on the community (required)
├── CMakeLists.txt           ESP-IDF project: board defaults, project()
├── sdkconfig.defaults       this firmware's settings, after the HAL's board defaults
├── partitions.csv           a table that keeps the 203 Launcher (or the HAL's board/partitions.csv)
├── version.txt              major.minor.patch: the version in the app description
├── CHANGELOG.md             "## <version>" entries: the release notes
├── LICENSE                  your code's licence
├── README.md                for developers: what it is, how to build and try it
├── docs/
│   ├── description.md       the page's description: short, for people deciding to install
│   └── screens/             3–5 screenshots of it: the page's gallery (`203cli board screen`)
├── .gitignore
├── dependencies.lock        written by the first build: commit it
├── components/
│   └── CardenzaHAL/         git submodule
└── main/
    ├── CMakeLists.txt
    └── main.cpp             app_main
```

### Start a project

```sh
203cli new "My Firmware" --summary "What it does, in a line"
```

It makes `my-firmware/` from the template (it builds as it is, for the board and the browser, and passes the
community's checks): a git repository, the HAL as the submodule `components/CardenzaHAL`, CMake's `project()`
named after the firmware (`my_firmware`: the BIN is `build/my_firmware.bin`), `203.json`, a README that says how to
build, try and publish it, and the release workflow. Nothing is committed or published. Then add a `LICENSE`, write
`main/main.cpp`, and commit. To change someone else's firmware instead, use `203cli fork`
([Forks](#7-forks-and-provenance)).

By hand, the same:

```sh
git clone --depth 1 https://github.com/203-Systems/CardenzaHAL.git /tmp/CardenzaHAL
cp -r /tmp/CardenzaHAL/examples/template my-firmware
cd my-firmware && git init
git submodule add https://github.com/203-Systems/CardenzaHAL.git components/CardenzaHAL
```

then rename `my_firmware` in `CMakeLists.txt`, `203.json` (`publish.bins`) and `.github/workflows/firmware.yml`
(`elf:`), and fill in `203.json`.

### `203.json`

One file at the project's root, committed with the source: what the firmware is called, its version, who
makes it, what it was made from, and what `203cli publish` uploads. JSON, so CMake, Python, Node and an AI
read it without anything installed. `203cli init` writes it (from the CMake `project()`, `version.txt` or the
git tag, the signed-in account, the git remote and `LICENSE`); `203cli publish` writes the firmware's `code`
back into it after the first publish: commit that.

```json
{
  "format": 1,
  "name": "My Synth",
  "code": "K7M2QX",
  "author": "ada",
  "summary": "A four-track groovebox: drums, Juno, DX7, sampler",
  "tags": ["groovebox"],
  "devices": ["cardenza"],
  "source": "https://github.com/me/my-synth",
  "license": "MIT",
  "forked_from": {"code": "V6QK28", "version": "0.2.0"},
  "publish_source": true,
  "publish": {"channel": "stable", "bins": {"cardenza": "build/my_synth.bin"}, "notes_file": "CHANGELOG.md",
              "cover": "docs/cover.png", "description_file": "docs/description.md",
              "images": ["docs/screens/home.png", "docs/screens/seq.png", "docs/screens/mix.png"]}
}
```

| Key | Value | |
| --- | --- | --- |
| `format` | `1` | required |
| `name` | the firmware's name on the community, 1–64 characters | required |
| `code` | the firmware's 6-character code; `null` until the first publish (203cli writes it) | |
| `version` | the version label (`[0-9A-Za-z._+-]`, 1–32). Leave it out when the project has `version.txt` (that file is then the version); when both are there they must agree | `version` or `version.txt` |
| `author` | your community name (informational: the server knows the owner; 203cli warns when it is not the signed-in account) | |
| `summary` | the cover blurb, 1–100 characters: what it does, not just its name | for the first publish |
| `tags` | up to 8: `instrument`, `synth`, `drums`, `sampler`, `groovebox`, `sequencer`, `tool`, `network`, `game`, … | |
| `devices` | the devices it is built for (`cardenza`, …) | default `["cardenza"]` |
| `source` | the repository's page (`https://…`); its clone link is the same with `.git` on GitHub, GitLab, Gitee and Codeberg | |
| `license` | an SPDX identifier (`MIT`, `Apache-2.0`, `GPL-3.0-only`, …) or `proprietary` | |
| `forked_from` | `{"code", "version"}`: the firmware and version this one was made from; absent or `null` for an original | for forks |
| `publish_source` | `true`: every published version carries a source archive of the commit | default `false` |
| `publish` | what `203cli publish` reads: `channel`, `bins` (`{device: path}`), `notes_file`, `cover`, `description_file` (the page's description: `docs/description.md`, [The page](#the-page-a-short-description-and-35-screenshots)), `images` (the gallery: 3–5 screenshots, each a path or `{"file", "caption"}`), `notices`, `sim` (a built web folder), `project` (the ESP-IDF project folder), `private` | |
| `$schema`, `x-…` | for editors and tools; ignored | |

Never a secret in it: no token, no password (203cli refuses the file). Older projects may have `203cli.json`;
`203cli init` moves it over.

CMake may read it too (CMake 3.19+). ESP-IDF already takes the version from `version.txt`, so read
`version` only when there is no `version.txt`, and check for the key: without it `string(JSON …)` yields
`version-NOTFOUND`. In the top-level `CMakeLists.txt`, before `project()`:

```cmake
file(READ "${CMAKE_CURRENT_LIST_DIR}/203.json" project_json)
string(JSON FIRMWARE_NAME GET "${project_json}" name)
string(JSON json_version ERROR_VARIABLE no_version GET "${project_json}" version)
if(NOT no_version AND NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/version.txt")
    set(PROJECT_VER "${json_version}")    # the app description's version
endif()
```

### `CMakeLists.txt`

Top level (the template's, with the HAL as a submodule):

```cmake
cmake_minimum_required(VERSION 3.16)
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/components/CardenzaHAL/board/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(my_firmware)
```

`main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "." REQUIRES CardenzaHAL)
```

- The project's name is the BIN's name (`build/my_firmware.bin`) and the app description's project; the community and the Launcher show `203.json`'s `name`.
- The HAL is C++17: use `.cpp` for code that includes its headers.
- The same `CMakeLists.txt` files build for the browser: the web build sets `IDF_PATH` to the HAL's ESP-IDF shim and runs them as they are.
- Optional: [`cmake/firmware_info.cmake`](https://github.com/203-Systems/CardenzaHAL/blob/main/cmake/firmware_info.cmake) checks `version.txt` (`cardenza_read_version`) and generates a header with the version, the Git commit, whether it was built with local changes, and the build time (`cardenza_target_build_info`): print them at every start.

### `sdkconfig.defaults`

List the HAL's [`board/sdkconfig.defaults`](https://github.com/203-Systems/CardenzaHAL/blob/main/board/sdkconfig.defaults)
first and your own after it (as above); yours win where they differ. The board's file sets what the board
is; keep these:

| Setting | Why |
| --- | --- |
| `CONFIG_IDF_TARGET="esp32s3"` | the chip (the checker refuses any other) |
| `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y`, `CONFIG_ESPTOOLPY_FLASHFREQ_120M=y` | the 203 Launcher's bootloader runs the flash in QIO; build so (DIO code ran ~40% slower) |
| `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` | the chip's flash (16 MB). Under the Launcher the app sees the size the Launcher's bootloader declares (8 MB on a board flashed with Launcher 0.3.3 or earlier) and gets at most 0x690000 bytes of it; leave `CONFIG_SPI_FLASH_SIZE_OVERRIDE` off |
| `CONFIG_SPIRAM=y`, `_MODE_QUAD`, `_SPEED_120M`, `_IGNORE_NOTFOUND=y` | the 2 MB PSRAM (ESP32-S3R2); how the heap uses it is yours (`CONFIG_SPIRAM_USE_*`, see [examples/psram_heap](https://github.com/203-Systems/CardenzaHAL/tree/main/examples/psram_heap)) |
| `CONFIG_FREERTOS_HZ=1000` | millisecond delays and frame periods (`Display::FramePacer`); the display no longer needs it |
| `CONFIG_FATFS_SECTOR_512=y`, `CONFIG_FATFS_LFN_HEAP=y` | the SD card |

Yours adds at least the partition table:

```text
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```

(or `"components/CardenzaHAL/board/partitions.csv"` to use the HAL's file directly).

- Keep the default console (UART0, with USB-Serial-JTAG as its second output) and the default log level (INFO): the community's boot check reads that log. A console only on USB prints nothing there (a warning, see [Checks](#5-what-the-community-checks)). On the board `CardenzaHAL::Init` starts USB and the console follows it to the HAL's system channel (where `203cli log` reads it); in the boot check's emulator there is no USB and it stays where it is.
- No secure boot or flash encryption: the Launcher installs plain, unsigned app images.
- Debug builds may add [`board/sdkconfig.debug`](https://github.com/203-Systems/CardenzaHAL/blob/main/board/sdkconfig.debug) (a hung task restarts the board with a backtrace).
- Commit `sdkconfig.defaults`, not the generated `sdkconfig`. Never put a Wi-Fi password or any secret into either.

### The partition table: keep the 203 Launcher

The device's partition table belongs to the 203 Launcher. It installs a firmware into a slot it makes,
after its own partitions, and never uses the firmware's table. The firmware's table only sizes the image
and matters when a board without the Launcher is flashed directly; then it must keep the Launcher's
regions exactly where they are. Use the HAL's
[`board/partitions.csv`](https://github.com/203-Systems/CardenzaHAL/blob/main/board/partitions.csv)
(the template keeps a copy as `partitions.csv`):

```text
# Name,    Type, SubType,  Offset,   Size
nvs,       data, nvs,      0x9000,   0x5000
otadata,   data, ota,      0xe000,   0x2000
launcher,  data, 0x40,     0x10000,  0x150000
coredump,  data, coredump, 0x160000, 0x10000
ota_0,     app,  ota_0,    0x170000, 0x400000
```

- Never put anything else below `0x170000`, never move `nvs`, `otadata`, `launcher` or `coredump`.
- The image must fit the Launcher's room for one app on the Cardenza: **0x690000 bytes (6.56 MB) from 0x170000**, counted as the image's size rounded up to 64 KB. That is what every Cardenza can hold: the flash is 16 MB, but the Launcher uses only what its bootloader declares, and boards flashed with Launcher 0.3.3 or earlier declare 8 MB until their bootloader is written again over USB. `ota_0` (4 MB) makes `idf.py build` fail earlier; an image beyond 4 MB may grow `ota_0` up to `0x690000`, no further.
- The firmware never writes the partition table, the otadata or the Launcher's slot, and never calls `esp_ota_set_boot_partition`.

### `version.txt`

One line, `major.minor.patch` (`0.1.0`). ESP-IDF reads it into the app description (the version the
Launcher shows and the community's checks read), the web build reads it into `sim.json`, and 203cli reads it
as the version label. Raise it for every version you publish on stable or beta. Without `version.txt`,
put `version` in `203.json` and `PROJECT_VER` in CMake ([above](#203json)).

### `LICENSE`, `README.md`, `CHANGELOG.md`

- `LICENSE`: your code's licence, with its SPDX identifier in `203.json` `license`. A fork keeps the original's licence terms ([Forks](#7-forks-and-provenance)). Code you vendor keeps its own licence and notices (`publish.notices` uploads a notices file with the BIN).
- `README.md`: for developers: what the firmware is, how to build and try it, how its source is laid out. It is not the firmware's page: a README with every key's table and the build steps made a 13,000-character page nobody read to the end (2026-11-20). The page's description is `docs/description.md` ([The page](#the-page-a-short-description-and-35-screenshots)); the keys go in the firmware's tutorial on the community.
- `CHANGELOG.md`: one `## <version>` entry per version, newest first. 203cli takes a version's release notes from its entry; a stable version needs notes.

```markdown
## 0.2.0

- Song mode: chain patterns.
- Fixed: the arpeggiator stopped after a pattern change.
```

### `.gitignore`

```gitignore
build/
build-*/
.203/
sdkconfig
sdkconfig.old
managed_components/
.env
.env.*
!.env.example
*.pem
*.key
*.p12
*.pfx
```

`build-web/` (the web build) is covered by `build-*/`; `.203/` is 203cli's working folder (`203cli sync` writes
`.203/community.md` there). Commit `dependencies.lock`.

## 3. Using the HAL

The HAL owns the hardware; the firmware owns everything above it (rendering, key mappings, audio
processing, file formats, the main loop). The rules (also in the HAL's
[AGENT.md](https://github.com/203-Systems/CardenzaHAL/blob/main/AGENT.md)):

- **Public headers only**, included from the HAL's root:

  | Header | What |
  | --- | --- |
  | `cardenzaHAL.h` | `CardenzaHAL::Init()`, `LastInitReport()` |
  | `system/system.h` | the shared I2C bus (`GetI2CBus`), the HOME button (`GetHomeButton`, the side key) |
  | `system/menu.h` | the system menu: a press of HOME opens it in every firmware (Continue, Settings, Back to Launcher, Restart, Turn off, About this device, Download mode); `SetListener`, `SetLeaveHook`, `IsOpen`, `Enable(false)` |
  | `system/settings.h` | the board's settings, the same in every firmware: volume and its limit, screen brightness, key lights, language (`Get`, `Set`, `Describe`, `SetListener`) |
  | `keyboard/keyboard.h`, `keyboard/types.h` | the 4 × 14 keys (`updateKeyList`, `keyList`), key LEDs (`setLED`, `setLEDFrame`, `setLEDGlobalBrightness`, `enableLED`) |
  | `keyboard/legends.h` | what each key cap says: its name, its character and shifted character, the arrows (`Legend`, `Typed`, `kKeyUp`, `kKeyOk`, …; a plain header) |
  | `keyboard/led_light.h`, `keyboard/key_settle.h` | perceived brightness to LED level; a filter for modifier keys (plain headers) |
  | `display/display.h`, `display/region.h`, `display/changed_regions.h` | the 240 × 135 screen (`Present`, `PresentChanged`, `SetBrightness`, `GetPanel`, `GetPanelIO`) |
  | `display/presenter.h`, `display/scan_order.h`, `display/frame_pacing.h` | the frame presenter: `FramePacer` (a loop's ticks and frames), `PresentOptions`, `RequestWholeFrame`, what each frame cost (`LastFrame`, `Totals`, `SetFrameCallback`), `AllocateFramebuffer`, `DoubleBuffer` ([docs/DISPLAY.md](https://github.com/203-Systems/CardenzaHAL/blob/main/docs/DISPLAY.md)) |
  | `audio/audio.h` | the speaker's I2S TX channel (`Init` mono / stereo, `GetTxChannel`, `SetVolume`) |
  | `mic/mic.h` | the PDM microphone's RX channel, and the gain for what you read (`ApplyGain`, `mic/gain.h`: your firmware's, from its own settings) |
  | `storage/storage.h`, `storage/app_dir.h` | the SD card at `/sdcard` (`Init`, `IsMounted`, `CheckAfterIoError`); your firmware's folder on it, `/sdcard/Apps/<AppName>/` (`AppDir`, `AppPath`, `EnsureAppDir`, `MigrateAppDir`) |
  | `ir/ir.h` | IR transmit (`Send`) |
  | `usb/usb.h` | USB MIDI, audio and HID, the app channel, the system commands |
  | `board/pins.h` | the pin map (reference: the HAL owns these pins) |

- **Never re-implement what the HAL provides**: no keyboard scan, LED driver, display or codec setup, SD mount or USB stack of your own. The screen: draw into a framebuffer and present it with `Display::PresentChanged` in a `Display::FramePacer` loop (only what changed goes out, in the panel's scan order: [docs/DISPLAY.md](https://github.com/203-Systems/CardenzaHAL/blob/main/docs/DISPLAY.md)), not with change finding or DMA of your own. Use its native ESP-IDF handles (`i2s_chan_handle_t`, `esp_lcd_panel_handle_t`, the I2C bus) for what it leaves to you.
- **Start with `CardenzaHAL::Init()`** and check it (`LastInitReport()` says which module failed). Its log lines (tag `CardenzaHAL`) are how the community recognises a firmware built with the HAL; a missing SD card or codec must not stop the firmware or restart it.
- **Do not reconfigure what the HAL owns**: I2C0 (GPIO1/2), I2S1 and I2S0 (GPIO41/42/43/46; GPIO43 is shared by the speaker and the microphone), LEDC timers 0/1 and channels 6/7, GPIO47/48/21 (key LEDs), GPIO0 (HOME; held at a reset it is the ROM download mode), the USB PHY.
- **HOME is the system's**: a press of HOME (the side key) opens the system menu (`system/menu.h`), the way back to the Launcher from any firmware. Do not give HOME another meaning, and do not turn the menu off (`SystemMenu::Enable(false)`, `CONFIG_CARDENZA_HAL_SYSTEM_MENU`) in a firmware for users. While the menu is open your frames are not sent and your keys read released: pause what must not run unseen (`SystemMenu::SetListener`), save before the board leaves (`SystemMenu::SetLeaveHook`).
- **The volume, the brightness, the key lights and the language are the user's**: the board's settings (`system/settings.h`, HOME > Settings, [docs/SETTINGS.md](https://github.com/203-Systems/CardenzaHAL/blob/main/docs/SETTINGS.md)), which `CardenzaHAL::Init` applies. Do not set them yourself at start (`Audio::SetVolume`, `Display::SetBrightness`, `Keyboard::setLEDGlobalBrightness`: that overrides the user's choice) and do not keep your own copies; a level of your own (a mixer's master) stays yours, before the board's volume. Read them with `Settings::Get`, offer a shortcut with `Settings::Set`. The microphone's gain is yours, not the board's: keep it with your own
  settings and pass what you record through `Mic::ApplyGain(samples, count, gainDb)`.
- **Keys do what their caps say** ([`keyboard/legends.h`](https://github.com/203-Systems/CardenzaHAL/blob/main/keyboard/legends.h); the map: the [README](https://github.com/203-Systems/CardenzaHAL/blob/main/README.md#keyboard-and-key-leds)). The arrows are the yellow triangles on `;` up, `.` down, `,` left, `/` right: in text input fn + those keys, on a screen that types nothing (a list, a menu, a game) the keys alone, as the 203 Launcher and the system menu do. ok confirms, esc (or del) goes back, and in text input each key types its printed character (with shift the yellow one; del erases). Never invent a layout: no HJKL, IJKL or WASD arrows, no other keys for confirm or back. The left column is esc, tab, fn, ctrl from the top (fn is the third key, 29; the bottom-left one is ctrl); HOME, the side key, is the system menu's.
- **ESP-IDF only**: ESP-IDF and FreeRTOS APIs, C++17. No Arduino core or other framework on top: it would set up the hardware and TinyUSB itself, and the web target does not provide it.
- **No copyleft code in the HAL**, and know what you vendor into your firmware: its licence becomes part of yours.
- **Your files go in your firmware's folder on the SD card, `/sdcard/Apps/<AppName>/`** ([docs/SD-CARD.md](https://github.com/203-Systems/CardenzaHAL/blob/main/docs/SD-CARD.md), the layout every firmware shares): `Storage::AppDir()`, `Storage::AppPath("Settings/app.cfg")`, `Storage::EnsureAppDir("Settings")` (`storage/app_dir.h`). `Settings/` and `Logs/` are the usual sub-folders; the rest is yours. Never write at the card's root, in `/sdcard/Launcher/` (the 203 Launcher's) or in another firmware's folder. The folder is named after your CMake project (`project()`), or `CONFIG_CARDENZA_HAL_APP_NAME`: choose it once. A firmware that kept its files elsewhere before calls `Storage::MigrateAppDir("/sdcard/<old folder>")` at the start, before reading any (one rename; a merge that never overwrites when both are there; safe against a power cut). The template does this right.
- **Under the 203 Launcher**: do not use NVS if you can avoid it (a flash write stops both cores' caches for ~45 ms), and if you must, use your own namespace: never erase the NVS partition and never write the Launcher's namespaces (`l_*`); return to the Launcher with `System::RestartIntoLauncher()` (`system/launcher.h`: a plain `esp_restart()` starts your firmware again when the Launcher's "Resume after reset" is on). To open the Launcher by hand, hold fn (the left column's third key, 29) while the board starts; not HOME: held at a reset it starts the ROM's download mode.
- What the chip does that is easy to miss (two RAMs, atomics with PSRAM, `memcpy` and division in hot code, files on the card, USB and the serial port): [docs/developing.md](https://github.com/203-Systems/CardenzaHAL/blob/main/docs/developing.md).

### The web build follows the HAL too

The web target provides exactly what a firmware may use: the HAL's API and its native handles, FreeRTOS
and the ESP-IDF system services (the [README's table](https://github.com/203-Systems/CardenzaHAL/blob/main/README.md#what-follows-the-hal-means)).
Anything else (Wi-Fi, Bluetooth, UART, ADC, other drivers, registers, `esp_private/*`, assembly,
prebuilt libraries) fails the web build with exit code 3 and one line per offender
(`unsupported: <function>`, `unsupported-header: <header>`, `unsupported-source: <file>`). Fix those by
going through the HAL; do not hide them.

One task never yielding (a loop that calls no FreeRTOS, ESP-IDF or HAL function) holds up the whole
simulator: block on queues or delays, as on the board.

`CARDENZA_HAL_WEB` tells the web build apart, for the rare place that must differ (a vendored library
with its own web code keyed on `__EMSCRIPTEN__`, a feature the web target does not have). CMake has the
variable, the code the definition of the same name:

```cmake
# main/CMakeLists.txt
if(CARDENZA_HAL_WEB)
    set(net_sources "net_none.cpp")      # no Wi-Fi in the browser
else()
    set(net_sources "net_wifi.cpp")
endif()
idf_component_register(SRCS "main.cpp" ${net_sources} INCLUDE_DIRS "." REQUIRES CardenzaHAL)
```

```cpp
#ifdef CARDENZA_HAL_WEB
    ESP_LOGI(kTag, "running in the simulator: no network");
#else
    StartNetwork();
#endif
```

Never put `CARDENZA_HAL_WEB` into the HAL itself, and keep such branches few: the simulator version is
there to show the real firmware.

## 4. Build

### The device

From the project's root:

```sh
python3 components/CardenzaHAL/tools/cardenza.py build      # the preferred way; --web builds the browser version too
```

The HAL's build tool checks first what a build needs (ESP-IDF 5.3, git, a clean HAL submodule, `203cli` installed;
`doctor` lists it all with the command that installs each), exports ESP-IDF when the shell has not, runs
`idf.py build` and says how much of the Launcher's room the image takes. In an activated ESP-IDF 5.3.5 shell,
`idf.py build` alone builds the same.

- The BIN to install and publish is **`build/<project>.bin`** (the application image). Not `bootloader/bootloader.bin`, not a merged image, not the `.elf`.
- Keep the `.elf` of every BIN you give anyone: a crash's backtrace is read with it (`xtensa-esp32s3-elf-addr2line -pfiaC -e build/<project>.elf <addresses>`).
- The build fails when the image does not fit the partition table's app partition.

### Trying it on a board

Most boards run the 203 Launcher. **Never `idf.py flash` a board that has it** (ESP-IDF suggests it after every
build): a full flash writes your bootloader and partition table, which erases the Launcher and its list of
installed firmware. Install through the Launcher, the way users get it:

```sh
203cli flash --bin build/<project>.bin                   # over the USB cable; no sign-in
203cli log                                               # what it prints (Ctrl-C stops; a program: --seconds)
203cli log --until "my_firmware 0.1.0 ready" --seconds 30  # a test: exit 0 at that line, 1 at a panic or when it never comes
```

- `flash` finds the device in its Launcher or running a firmware (a firmware on the HAL is restarted into its Launcher first), writes the image through it and the Launcher starts it. An image of a firmware the device has replaces that copy, and 203cli says so.
- `log` reads a running firmware's console over the HAL's USB system channel (every firmware on the HAL: `CardenzaHAL::Init` starts USB), else the serial port (the Launcher, a firmware without USB). The first lines are the last kilobyte the firmware printed before you asked.
- `203cli board` asks a running firmware about itself over the same channel, with no code of the firmware's: `info` (what it is, how it was built, the Launcher, the chip, why it last restarted), `partitions` (the table on the board and how full each is), `memory` (RAM, PSRAM, each task's stack and CPU), `storage`, `status`, `i2c`, `screen` (a PNG of its framebuffer), `press` (keys pressed from the computer: test a screen without touching the board), `coredump` (the last crash) and `files` (`ls`, `get`, `put`, `rm`, `mkdir`, `mv` on the SD card). Every firmware on the HAL has it (`CardenzaHAL::Init` starts USB; `CONFIG_CARDENZA_HAL_USB_AUTOSTART`), over HID, SysEx, WebUSB or a serial port, whichever the firmware's USB classes have. Tasks, CPU shares and core dumps need the board's debug defaults (`board/sdkconfig.debug`). The protocol: [docs/usb-dev.md](https://github.com/203-Systems/CardenzaHAL/blob/main/docs/usb-dev.md).
- Windows: the website installs a local BIN in Chrome or Edge (the firmware section's "Flash a local file"); the log: `pip install hidapi`, then `python components/CardenzaHAL/tools/cardenza_usb.py console 60`.
- Other ways: copy `build/<project>.bin` to the SD card and install it from the Launcher's file browser; the HAL's [uploader](https://github.com/203-Systems/CardenzaHAL/blob/main/tools/uploader/README.md) (`serial_flasher.py`, from the Launcher's home screen); [`cardenza_flash.py`](https://github.com/203-Systems/CardenzaHAL/blob/main/tools/cardenza_flash.py) `--app-into <label>` (only the app into one of the Launcher's slots; it refuses a full flash when the Launcher is there); `cardenza_usb.py command launcher|download` restarts a firmware into the Launcher or the ROM's download mode.
- Released firmware reaches people through the community (`203cli publish`), not as files to flash.

### The browser (the simulator version)

```sh
python3 components/CardenzaHAL/tools/cardenza_web.py build .
python3 components/CardenzaHAL/tools/cardenza_web.py serve build-web/site
```

- `build` compiles the same project with Emscripten against the HAL's web target and writes **`build-web/site`** (`--out` to choose): `index.html`, `<project>.js`, `<project>.wasm`, `cardenza-sim.js` and `sim.json` (its manifest: the target, the firmware's name and version, the HAL's version and commit and whether it had local changes, the Emscripten version, the key map, every file's size and SHA-256). CMake's build folder is `build-web/cmake`.
- It uses the firmware's own copy of the HAL when that has the web target (`components/CardenzaHAL/web`); `--hal <dir>` names another; `--debug` builds with assertions. A HAL submodule without `web/` and `tools/cardenza_web.py` is too old: update it.
- It reads the project's `sdkconfig` when there is one (a device build's), else `SDKCONFIG_DEFAULTS`.
- Exit codes: `0` built; `1` your code does not build; `2` not an ESP-IDF project; `3` the firmware does not follow the HAL (the list follows); `4` Emscripten or CMake is missing.
- `serve` opens <http://localhost:8000/> (`--port`), with the headers a host must send, and the same page framed the way the community shows it (`<iframe sandbox="allow-scripts">`, from another origin) at `http://localhost:8001/__sandbox.html`. The computer's keyboard plays the device's keys by position; `?log=1` shows the console under the board, `?sd=0` starts without a card.
- `203cli publish` runs this build itself and uploads the folder with the BIN.

## 5. What the community checks

Every uploaded version is checked automatically before it is published. A version whose `image`, `layout`
or `boot` step fails is **sent back** (打回) to you with the report and is not published; warnings never
send it back but show as tags on the firmware.

| Step | Passes when | Fails (sent back) | Warns |
| --- | --- | --- | --- |
| `image` | a valid ESP-IDF application image for the ESP32-S3 with its app description (project, version, IDF), nothing after it, up to 16 MB | another chip, no app description, trailing bytes, a merged full-flash image, the 203 Launcher | |
| `layout` | the image rounded up to 64 KB fits the Launcher's room for apps: Cardenza 0x690000 bytes from 0x170000 | too large | a device the checker does not know |
| `boot` | booted in Espressif's QEMU (ESP32-S3, 16 MB flash, 2 MB PSRAM) for 20 s the way the Launcher starts it (a QIO bootloader, the Launcher's table plus `ota_0`): `app_main` reached, no panic, watchdog reset or restart | the bootloader refuses the app; a panic or abort before `app_main`; a panic after it with no hardware error before; a watchdog reset; two or more restarts (a boot loop) | a panic after hardware errors (missing hardware, most likely); one restart; the task watchdog complaining; nothing printed after the bootloader (a console on USB, logs off, a hang) |
| `hal` | the HAL's marker `CardenzaHAL/<version>+<commit>` in the image (HAL versions that carry it), or its log tag `CardenzaHAL` in the image or the boot log (version unknown) | never | neither: the `no_hal` tag |
| `simulator` | the web build, opened in a headless browser in `<iframe sandbox="allow-scripts">` from another origin, posts `{"cardenza_sim":"ready"}` within 15 s | never | no message in time or a page that does not load: `simulator_fails`, no Play mark. No web build at all: `no_simulator` |

What that asks of the firmware:

- The emulator has no I2C, I2S, SPI display, LEDC, RMT, SD card, USB, Wi-Fi or Bluetooth. Errors about them are listed, not counted. So treat missing hardware as a reported error, not as a reason to `abort()`, `ESP_ERROR_CHECK` a driver call before `app_main`, or restart: a firmware that waits for its display passes if it reached `app_main` cleanly.
- Keep the boot log on UART0 with INFO logs (the defaults). It is how `boot` and `hal` see your firmware run.
- Call `CardenzaHAL::Init()`: its log tag is the HAL's fingerprint.
- The HAL's web page posts the ready message by itself as soon as `app_main` runs (`web/page/cardenza-sim.js`); `app_main` must therefore be reached within 15 s. The build may load only its own files (no other origins; the frame has no storage, MIDI or microphone).
- Try it before publishing: the web build with `cardenza_web.py serve` (the sandboxed page), the BIN on a board under the Launcher.

When a version is sent back: `203cli versions check <VCODE> --json` shows which step failed and the log.

- A real failure (a panic, a boot loop, too large): fix it and publish a **new** version label. A sent-back version keeps its label for 30 days.
- A false positive (the firmware only fails for want of hardware the emulator lacks): appeal with a message of 10–2,000 characters saying why (`203cli versions appeal <VCODE> --message "…"`, or on the version's page). It goes to the community's own tracker; an admin answers, publishes a false positive and fixes the checker.

## 6. Versions and channels

| Channel | For | |
| --- | --- | --- |
| `stable` | releases, published by hand | the default; the Launcher installs it unless the user chose another channel; release notes expected |
| `beta` | a big change to try | turned on by the author |
| `dev` | builds from CI, possibly many a day | turned on by the author; the newest 30 are kept |

- **Labels**: 1–32 characters of `0-9 A-Z a-z . _ + -` (`1.4.0`, `1.4.0-rc1`, `0.2.0-dev.57`), no leading `v` (203cli drops it). A label is unique within the firmware, across channels and devices, and is never reused: a version never changes.
- **Never lower than the newest on the channel**: the Launcher installs the newest by date, so a lower label would take people back. 203cli stops and asks for `--force` when you mean it.
- **The BIN says its own version** (its app description, from `version.txt`): on stable and beta the label must be that version (203cli stops otherwise); on dev it may add to it (`<version>-dev.<run>`), which keeps CI labels unique and in order.
- Every version gets its own 6-character code. The firmware's code installs its newest version on the channel chosen in the Launcher; a version's code installs exactly that version.
- One firmware may carry builds for several devices (`devices` in `203.json`); each device has its own newest version per channel.
- **From CI (the standard release)** the channel follows the Git ref, the same for every firmware:

  | Ref | Channel | Label |
  | --- | --- | --- |
  | a push to `main` | `dev` | `<version>-dev.<run number>` |
  | a tag `vX.Y.Z` (equal to `version.txt`) | `stable` | `X.Y.Z`, its notes `CHANGELOG.md`'s `## X.Y.Z` |
  | a tag `vX.Y.Z-beta.N` (`X.Y.Z` equal to `version.txt`) | `beta` | `X.Y.Z-beta.N`, its notes `## X.Y.Z-beta.N` if there is one, else a line naming the beta |

  So a release is: raise `version.txt`, add its `CHANGELOG.md` entry, merge to `main`, tag `vX.Y.Z`.
- Limits per account per day: 30 stable and beta versions, 300 dev versions. A draft not published within a day is removed.

## 7. Forks and provenance

What a firmware was made from is recorded and shown (`forked_from`, the firmware's tree, "made from X"),
so a fork is always traced back to its original.

- **Start a fork with `203cli fork <CODE> [DIR]`**: it clones the original's source, makes the fork (or finds the one you have) and writes `203.json` with `forked_from` (the original's code and its newest stable version) and the fork's own `code`. Example: `203cli fork V6QK28 my-synth` (Cardenza Synth, the official groovebox).
- `203cli publish` with a `203.json` whose `code` is someone else's firmware makes a fork of it first (yours, `forked_from` that code and version), rewrites `code` and `forked_from` in the file and says so. With `forked_from` and no `code`, the new firmware is made as a fork of it. Commit the rewritten file.
- Made from something but published as an original? Set it afterwards: `203cli firmware based-on <CODE> <ORIGIN> [--version <LABEL>]`, or the firmware's edit form. The original must be one you can see, not the firmware itself or one of its own forks, of the same kind. Its owner is notified.
- **Keep the original's licence**: its `LICENSE` file and copyright lines stay, your changes are under terms it allows, and `203.json` `license` says the result's licence. A firmware without a licence grants no right to redistribute it: ask its author first.
- **Give credit**: say in the README what the firmware was made from, with a link. Do not publish someone else's work as your original.
- Launchers are not forked: only the official 203 Launcher is offered, and only admins make or fork launchers.
- Official firmware are published the standard way too: their owner's CI publishes their versions with the same action (Cardenza Synth, `V6QK28`; the 203 Launcher). Only an official firmware mirrored from the old Launcher hub (no owner) takes no versions from 203cli (`official_from_hub`). Your fork is your own firmware with its own code.

## 8. Publish

Every firmware is released the same way, 203 Systems' own included: versions on the community's channels,
published by CI with the HAL's [`community-publish`](https://github.com/203-Systems/CardenzaHAL/blob/main/.github/actions/community-publish/action.yml)
action. The first version is published by hand, which makes the firmware and gives it its code:

```sh
python3 components/CardenzaHAL/tools/cardenza.py build   # the BIN (raise version.txt and CHANGELOG.md first)
203cli publish --dry-run      # everything checked, nothing changed: fix what it reports
203cli publish                # uploads, waits for the checks, prints the codes and the pages
git add 203.json && git commit -m "203.json: the firmware's code"   # after the first publish
203cli link                   # once the repository is on GitHub: its CI publishes (below)
```

Publishing goes through 203cli, by hand or from CI (the action runs it). A project without `203.json` (not made by
`203cli new`): `203cli init` writes it first.

- `203cli publish` reads `203.json` (flags win over it): the firmware (`code`, or made from `name` and `summary` on the first publish), the version (`version.txt` or `version`), the channel (`publish.channel`, else stable), the BINs (`publish.bins`: `build/<project>.bin` for each device), the release notes (`CHANGELOG.md`'s `## <version>` entry).
- For a CardenzaHAL firmware it also builds the simulator version (`cardenza_web.py build`) and uploads it with the BIN, so the version shows Play once its web build boots. It needs Emscripten for that; without it, install it ([Setup](#emscripten-the-simulator-version)).
- With `"publish_source": true` it attaches a source archive of the commit (`git archive` of the tracked files): commit first, and nothing secret may be tracked (the server refuses the archive). `203cli source upload <VCODE>` adds one to an existing version.
- It waits for the checks and prints the firmware's and the version's codes, the version's page and its Play link; a version sent back makes it exit with an error ([Checks](#5-what-the-community-checks)).
- Signed in yourself it also needs the page below: the gallery it leaves must hold 3–5 different screenshots, and a description over 3000 characters is refused; both are checked before anything is uploaded.
- `203cli publish --json` answers in JSON (for agents and CI). `203cli help` lists every command; `203cli rules` prints this file.

### The page: a short description and 3–5 screenshots

People who are not developers decide on the firmware's page whether to install it: it reads like a product page,
not a manual. [The guide](https://cardenza.203.io/community/guide/description) (`203cli help description`) has the
template and an example.

- **The description**, `docs/description.md` (`publish.description_file`; `203cli new` and `203cli init` write it from the
  template: fill in every `<…>`): one or two sentences on what it is and who it is for, `## Features` with 3–6 one-line
  bullets, `## Needs` when it needs something (the Launcher's version, an SD card, an accessory), `## Get started` in
  1–3 lines, and a line pointing to its tutorial. Aim for 800 characters (pictures not counted); 203cli and the site warn
  over 1500, and a new or changed description over 3000 is refused. Not in it: key tables and every mode's keys (the
  tutorial), building it (the README), what changed (the release notes), long FAQs.
- **The gallery**, the large pictures above the description: 3–5 different screenshots of the firmware itself, the
  screen people see first and then its main features. With it running on a Cardenza on USB (every firmware on the HAL
  has it), `203cli board screen --out docs/screens/home.png` saves the
  screen as a PNG (the page enlarges it pixel for pixel; a firmware that draws through the panel's handles instead of
  `Present` has none: a photo then); list them in `publish.images` (or `203cli publish --image …`).
  `203cli publish` signed in yourself stops before uploading anything unless the gallery it leaves holds 3–5 pictures,
  all different (`gallery_pictures`); on the website they are only suggested. The browser simulator has no screenshot
  of its own.
- CI cannot change pictures (its credential publishes versions only), so a release never fails for them: 203cli warns
  (`gallery_short`) and the `community-publish` action shows it on the run; the owner adds them once, on the site
  (the firmware's edit page, 图片 / Pictures) or with their own sign-in.

### Publish from CI (the standard release)

**Trusted publishing**: no secret is stored anywhere. The job asks GitHub for its OIDC identity token, and
the community trades it for a credential that lasts 15 minutes and only publishes versions of this one
firmware, when the token comes from a repository (and workflow) the firmware's owner linked
(`203cli login --github-oidc --firmware <CODE>`; the action does it).

1. **Link the repository** (the firmware's owner, once): `203cli link` in the project (it also writes the page's
   source link and `203.json`'s; after the repository moved, `203cli link --move` unlinks the old one); or on the
   firmware's page, 自动发布 / Trusted publishing: the repository (`owner/name`), the workflow file (`firmware.yml`),
   optionally a GitHub environment, and the channels it may publish; or `203cli firmware publishers add <CODE> ...`
   (`203cli firmware publishers list <CODE>` shows them). A tag `vX.Y.Z` may then publish stable (and beta, dev), a tag `vX.Y.Z-beta.N` beta (and dev),
   any other ref dev only.
2. **Turn the beta and dev channels on** on the firmware's page: the CI credential publishes versions and
   nothing else (it changes no page, channel or cover).
3. **The workflow**: the template's [`.github/workflows/firmware.yml`](https://github.com/203-Systems/CardenzaHAL/blob/main/examples/template/.github/workflows/firmware.yml)
   (copied with the template). The publishing part:

```yaml
# .github/workflows/firmware.yml (excerpt)
on:
  push:
    branches: [main]
    tags: ['v*']
jobs:
  firmware:
    runs-on: ubuntu-latest
    container: espressif/idf:v5.3.5
    permissions:
      contents: read
      id-token: write          # trusted publishing: the job's OIDC token
    steps:
      - uses: actions/checkout@v4
        with: {submodules: recursive}
      - name: Build
        shell: bash
        run: |
          git config --global --add safe.directory '*'    # the container's user does not own the checkout
          . "$IDF_PATH/export.sh" > /dev/null
          idf.py build
      - name: Emscripten (the simulator version)
        run: |
          git clone --depth 1 https://github.com/emscripten-core/emsdk.git ~/emsdk
          ~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest
      # The HAL's action, at the HAL commit this firmware builds with (the submodule).
      - uses: ./components/CardenzaHAL/.github/actions/community-publish
        with:
          elf: build/my_firmware.elf
```

- The action takes the channel and the label from the ref (the table in [Versions and channels](#6-versions-and-channels)), the firmware's code and BINs from `203.json`, the version from `version.txt`, the notes from `CHANGELOG.md`; on another ref it does nothing. A stable tag that does not equal `version.txt`, or has no `CHANGELOG.md` entry, fails.
- It installs 203cli from the community, signs in with the job's OIDC token, publishes and waits for the checks (a version sent back fails the job, with the report); a run again resumes the same version, never a second one. It builds the simulator version when the runner has Emscripten (the step above), else publishes the BIN alone with a notice.
- `elf:` keeps the ELF as the run's artifact `elf-<label>` (90 days; the community does not take it). Keep the ELF of every release yourself too (a GitHub Release): a crash's backtrace needs it.
- Outside the HAL's submodule, `uses: 203-Systems/CardenzaHAL/.github/actions/community-publish@<a commit on main>`.
- Change the firmware's page (name, summary, tags, links, cover, pictures) on the site: CI does not touch it, and `203.json`'s page fields only matter for the first publish. A gallery without 3–5 different screenshots is a warning on the run (above), never a failure.
- **On your own Cardenza** (optional): a self-hosted runner on a computer with a Cardenza on its USB cable (labels `self-hosted, cardenza`) installs every build and checks that it starts; the template's workflow has the job (`on-device`, commented out): `203cli flash --bin build/<project>.bin` and `203cli log --until "<ready line>" --seconds 60`, which fails the job at a panic or when the line never comes.
- Not on GitHub Actions (or a repository that cannot be linked): a personal token (设置 › 个人令牌, `/me/tokens`, scope `write`) as the CI system's secret `COMMUNITY_TOKEN`, and `203cli publish --channel dev --version "$(tr -d '[:space:]' < version.txt)-dev.<n>" --commit <sha> --json` (the action's `token:` input does the same on GitHub).

### Users' feedback and forks

What the community has about a published firmware comes back to the project with `203cli sync` (in the project's
folder: the firmware is `203.json`'s): `.203/community.md` (and `community.json`) holds its page, every version and its
notes, the requests with their votes, status and discussion, every comment thread with its replies and likes, the merge
requests and the forks (魔改版), new since the last sync marked (git-ignore `.203/`; the template does). Read it before
planning a change. One part at a time: `203cli project show`, `project versions`, `project feedback --new`, `project
forks`; `203cli project diff <FORK>` compares a fork's source with this folder (`--patch` the unified diff), `203cli
project diff 1.0.0 1.1.0` two versions (their source archives: `publish_source: true`). Answer people with
`203cli comments add …` (they are notified); `203cli dev update …` keeps the internal tracker.

## 9. Never commit

Nothing secret goes into the repository, `203.json`, `sdkconfig.defaults`, the source or a published
source archive:

- 203 credentials: `cct_…` (CLI and CI tokens), `ccd_…` (a device's token), `cfu_…`, `upl_…`; the value of `COMMUNITY_TOKEN`; 203cli's token file in your home folder.
- `.env` and `.env.*` (`.env.example` and `.env.sample` may hold names without values), `.npmrc`, `.netrc`, `.git-credentials`, `credentials*.json`, files named `*secret*`.
- Keys and certificates: `*.pem`, `*.key`, `*.p12`, `*.pfx`, `id_rsa*`, `id_ed25519*`, `id_ecdsa*`, `id_dsa*`, any `-----BEGIN … PRIVATE KEY-----` block.
- Other services' tokens: GitHub (`ghp_`, `gho_`, `ghs_`, `github_pat_`), AWS key ids (`AKIA…`), Slack (`xoxb-` and the like), OpenAI-style keys (`sk-…`), Google API keys (`AIza…`).
- Wi-Fi names and passwords, server passwords, API keys of your own: not in the code, not in `sdkconfig.defaults` or a committed `sdkconfig`. A firmware asks the user, or uses what the Launcher set up on the device.
- A device's credential read on the board is never logged, printed or sent anywhere but the community.

203cli refuses a `203.json` that holds a secret, and the server refuses a source archive that has any of the
files or patterns above. If a secret was ever committed, revoke it (tokens on `/me`) and make a new one:
deleting the line does not remove it from the history.

## Checklist before publishing

- [ ] `tools/cardenza.py build --web` succeeds (the board and the browser); the BIN is `build/<project>.bin`.
- [ ] On a Cardenza: `203cli flash --bin build/<project>.bin` and `203cli log` show it starting, without a panic.
- [ ] `CardenzaHAL::Init()` runs first; missing hardware is reported, never aborts or restarts the firmware.
- [ ] The HAL submodule is clean and on upstream `main` (`check_submodule.py --fetch`), and current (`203cli updates`).
- [ ] The partition table keeps the Launcher's regions; the image is well under 0x690000 bytes.
- [ ] `version.txt` raised; `CHANGELOG.md` has its `## <version>` entry.
- [ ] `203.json`: `name`, `summary`, `tags`, `license`, `source`; `forked_from` for a fork.
- [ ] `LICENSE` and `README.md` present; a fork keeps the original's licence and credits it.
- [ ] `docs/description.md` by [the guide](https://cardenza.203.io/community/guide/description): what it is, 3–6 features, what it needs, how to start; no key tables, build steps or history (aim for 800 characters, at most 3000).
- [ ] 3–5 different screenshots of it in `publish.images` (`203cli board screen`).
- [ ] No secret tracked (`git ls-files` shows none of [the list](#9-never-commit)).
- [ ] `203cli publish --dry-run` reports nothing left to fix.
- [ ] Releases: the workflow with the `community-publish` action and `id-token: write`; the repository linked on the firmware's page (自动发布); beta and dev turned on if you use them.
