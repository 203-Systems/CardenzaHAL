#!/usr/bin/env python3
"""Builds the simulator version of a CardenzaHAL firmware: the same ESP-IDF project, compiled with
Emscripten for the HAL's web target (web/), into a page that runs it in a browser.

usage:
  python3 <HAL>/tools/cardenza_web.py build <firmware project> [--out DIR] [--build-dir DIR] [--hal DIR]
                                         [--debug] [--jobs N] [--verbose]
  python3 <HAL>/tools/cardenza_web.py serve <site folder> [--port N]

build configures the firmware's own CMakeLists.txt with IDF_PATH set to the web target's ESP-IDF build
shim (web/idf) and Emscripten's toolchain, builds it, and leaves in --out (default
<project>/build-web/site): index.html, <project>.js, <project>.wasm, cardenza-sim.js and sim.json (the
build's manifest). The HAL used is the firmware's own copy when it has the web target, else this one
(--hal names another).

Exit codes of build:
  0  built
  1  the firmware did not build (its own errors; the log is on stderr)
  2  usage, or not an ESP-IDF firmware project
  3  the firmware does not follow the HAL: it calls functions, includes headers or builds sources the
     web target does not provide; stderr lists them, one a line: "unsupported: <function>",
     "unsupported-header: <header>", "unsupported-source: <file>"
  4  Emscripten or CMake (3.20 or later) is missing; stderr says how to install them

serve runs a local web server for a built site with the headers a host must send (Access-Control-Allow-
Origin: *, application/wasm). It also serves /__sandbox.html on the next port, which shows the site in an
<iframe sandbox="allow-scripts"> from another origin, as the community's player does.

The CMake shim calls two more subcommands itself: "sdkconfig" (sdkconfig.h from the project's sdkconfig
or its defaults) and "assemble" (the page and sim.json next to the module).
"""

import argparse
import hashlib
import html
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import threading

HAL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

EXIT_BUILT = 0
EXIT_BUILD_FAILED = 1
EXIT_USAGE = 2
EXIT_UNSUPPORTED = 3
EXIT_TOOLCHAIN = 4

EMSCRIPTEN_HINT = """Emscripten was not found (emcmake). Install it once:
  git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
  ~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest
then run 'source ~/emsdk/emsdk_env.sh' in the shell that builds, or set EMSDK to the emsdk folder."""

CMAKE_HINT = """CMake 3.20 or later was not found. Install it (https://cmake.org/download, 'pip install cmake', or
the one ESP-IDF installs) and put it on PATH, or set CMAKE to its path."""


class ToolchainMissing(Exception):
    pass


# ---- sdkconfig ------------------------------------------------------------------------------------------

# ESP-IDF 5.3's defaults for what the web target reads, for a project with neither an sdkconfig nor
# defaults that say otherwise. Values as an sdkconfig writes them ("y", numbers, quoted strings).
IDF_DEFAULTS = {
    "CONFIG_IDF_TARGET": '"esp32s3"',
    "CONFIG_IDF_TARGET_ESP32S3": "y",
    "CONFIG_IDF_TARGET_ARCH_XTENSA": "y",
    "CONFIG_IDF_TARGET_ARCH": '"xtensa"',
    "CONFIG_IDF_FIRMWARE_CHIP_ID": "0x0009",
    "CONFIG_FREERTOS_HZ": "100",
    "CONFIG_FREERTOS_NUMBER_OF_CORES": "2",
    "CONFIG_FREERTOS_MAX_TASK_NAME_LEN": "16",
    "CONFIG_FREERTOS_IDLE_TASK_STACKSIZE": "1536",
    "CONFIG_FREERTOS_TIMER_TASK_PRIORITY": "1",
    "CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH": "2048",
    "CONFIG_FREERTOS_TIMER_QUEUE_LENGTH": "10",
    "CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES": "1",
    "CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS": "1",
    "CONFIG_FREERTOS_USE_TIMERS": "y",
    "CONFIG_LOG_DEFAULT_LEVEL_INFO": "y",
    "CONFIG_LOG_MAXIMUM_EQUALS_DEFAULT": "y",
    "CONFIG_LOG_TIMESTAMP_SOURCE_RTOS": "y",
    "CONFIG_ESP_MAIN_TASK_STACK_SIZE": "3584",
    "CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0": "y",
    "CONFIG_ESP_TIMER_TASK_STACK_SIZE": "3584",
    "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160": "y",
    "CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT": "y",
    "CONFIG_PTHREAD_TASK_PRIO_DEFAULT": "5",
    "CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT": "3072",
    "CONFIG_PTHREAD_STACK_MIN": "768",
    "CONFIG_PTHREAD_DEFAULT_CORE_NO_AFFINITY": "y",
    "CONFIG_PTHREAD_TASK_CORE_DEFAULT": "-1",
    "CONFIG_PTHREAD_TASK_NAME_DEFAULT": '"pthread"',
    "CONFIG_COMPILER_OPTIMIZATION_DEBUG": "y",
    "CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE": "y",
    "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL": "16384",
    "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL": "32768",
    "CONFIG_ESPTOOLPY_FLASHSIZE_2MB": "y",
    # CardenzaHAL's own Kconfig defaults (Kconfig). The presence service is on as on the device; the web
    # HAL has it absent (web/hal/presence.cpp).
    "CONFIG_CARDENZA_HAL_PRESENCE": "y",
    "CONFIG_CARDENZA_HAL_PRESENCE_START_DELAY": "10",
    # Empty: the firmware's folder on the card is named after its project (storage/app_dir.h).
    "CONFIG_CARDENZA_HAL_APP_NAME": '""',
    # The display's SPI clock: the web model times every transfer by it (web/src/drivers/lcd.cpp).
    "CONFIG_CARDENZA_HAL_DISPLAY_SPI_MHZ": "80",
}

# Kconfig choices: one member is y, the others are not set; some give a value of their own.
CHOICES = [
    ("SPIRAM_USE", ["CONFIG_SPIRAM_USE_MEMMAP", "CONFIG_SPIRAM_USE_CAPS_ALLOC", "CONFIG_SPIRAM_USE_MALLOC"], None, {}),
    ("ESP_MAIN_TASK_AFFINITY", ["CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0", "CONFIG_ESP_MAIN_TASK_AFFINITY_CPU1",
                                "CONFIG_ESP_MAIN_TASK_AFFINITY_NO_AFFINITY"], "CONFIG_ESP_MAIN_TASK_AFFINITY",
     {"CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0": "0x0", "CONFIG_ESP_MAIN_TASK_AFFINITY_CPU1": "0x1",
      "CONFIG_ESP_MAIN_TASK_AFFINITY_NO_AFFINITY": "0x7FFFFFFF"}),
    ("LOG_DEFAULT_LEVEL", ["CONFIG_LOG_DEFAULT_LEVEL_NONE", "CONFIG_LOG_DEFAULT_LEVEL_ERROR", "CONFIG_LOG_DEFAULT_LEVEL_WARN",
                           "CONFIG_LOG_DEFAULT_LEVEL_INFO", "CONFIG_LOG_DEFAULT_LEVEL_DEBUG", "CONFIG_LOG_DEFAULT_LEVEL_VERBOSE"],
     "CONFIG_LOG_DEFAULT_LEVEL",
     {"CONFIG_LOG_DEFAULT_LEVEL_NONE": "0", "CONFIG_LOG_DEFAULT_LEVEL_ERROR": "1", "CONFIG_LOG_DEFAULT_LEVEL_WARN": "2",
      "CONFIG_LOG_DEFAULT_LEVEL_INFO": "3", "CONFIG_LOG_DEFAULT_LEVEL_DEBUG": "4", "CONFIG_LOG_DEFAULT_LEVEL_VERBOSE": "5"}),
    ("PTHREAD_CORE", ["CONFIG_PTHREAD_DEFAULT_CORE_NO_AFFINITY", "CONFIG_PTHREAD_DEFAULT_CORE_0", "CONFIG_PTHREAD_DEFAULT_CORE_1"],
     "CONFIG_PTHREAD_TASK_CORE_DEFAULT",
     {"CONFIG_PTHREAD_DEFAULT_CORE_NO_AFFINITY": "-1", "CONFIG_PTHREAD_DEFAULT_CORE_0": "0", "CONFIG_PTHREAD_DEFAULT_CORE_1": "1"}),
    ("CPU_FREQ", ["CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_80", "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160", "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240"],
     "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ",
     {"CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_80": "80", "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160": "160",
      "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240": "240"}),
    ("COMPILER_OPTIMIZATION", ["CONFIG_COMPILER_OPTIMIZATION_DEBUG", "CONFIG_COMPILER_OPTIMIZATION_SIZE",
                               "CONFIG_COMPILER_OPTIMIZATION_PERF", "CONFIG_COMPILER_OPTIMIZATION_NONE"], None, {}),
]

_SET = re.compile(r"^\s*(CONFIG_[A-Za-z0-9_]+)\s*=\s*(.*?)\s*$")
_NOT_SET = re.compile(r"^\s*#\s*(CONFIG_[A-Za-z0-9_]+) is not set\s*$")


def parse_sdkconfig(text):
    """An sdkconfig or sdkconfig.defaults as {name: value or None}: None is "not set" (or =n)."""
    values = {}
    for line in text.splitlines():
        match = _SET.match(line)
        if match:
            name, value = match.groups()
            values[name] = None if value == "n" else value
            continue
        match = _NOT_SET.match(line)
        if match:
            values[match.group(1)] = None
    return values


def merge_sdkconfig(layers, base=None):
    """Later layers win, as SDKCONFIG_DEFAULTS files do; a choice member set to y unsets its others."""
    merged = dict(IDF_DEFAULTS if base is None else base)
    for layer in layers:
        for name, value in layer.items():
            for _, members, _, _ in CHOICES:
                if name in members and value == "y":
                    for other in members:
                        if other != name:
                            merged[other] = None
            merged[name] = value
    return complete_sdkconfig(merged)


def complete_sdkconfig(values):
    """What Kconfig would add: the values choices give, and PSRAM in malloc when nothing else is chosen."""
    values = dict(values)
    if values.get("CONFIG_SPIRAM") == "y" and not any(
            values.get(name) == "y" for name in ("CONFIG_SPIRAM_USE_MEMMAP", "CONFIG_SPIRAM_USE_CAPS_ALLOC", "CONFIG_SPIRAM_USE_MALLOC")):
        values["CONFIG_SPIRAM_USE_MALLOC"] = "y"
    for _, members, derived, chosen in CHOICES:
        if derived is None:
            continue
        for member in members:
            if values.get(member) == "y" and member in chosen:
                values[derived] = chosen[member]
    if values.get("CONFIG_LOG_MAXIMUM_EQUALS_DEFAULT") == "y" and "CONFIG_LOG_DEFAULT_LEVEL" in values:
        values["CONFIG_LOG_MAXIMUM_LEVEL"] = values["CONFIG_LOG_DEFAULT_LEVEL"]
    return values


def render_sdkconfig_h(values):
    lines = ["/* sdkconfig.h for the CardenzaHAL web target, made by tools/cardenza_web.py. */", "#pragma once"]
    for name in sorted(values):
        value = values[name]
        if value is None:
            continue
        lines.append("#define %s %s" % (name, "1" if value == "y" else value))
    return "\n".join(lines) + "\n"


def render_sdkconfig_cmake(values):
    lines = ["# sdkconfig.cmake for the CardenzaHAL web target, made by tools/cardenza_web.py."]
    for name in sorted(values):
        value = values[name]
        if value is None:
            lines.append('set(%s "")' % name)
            continue
        if value.startswith('"') and value.endswith('"') and len(value) >= 2:
            value = value[1:-1]
        lines.append('set(%s "%s")' % (name, value.replace("\\", "\\\\").replace('"', '\\"')))
    return "\n".join(lines) + "\n"


def make_sdkconfig(sdkconfig_path, defaults_paths):
    """The project's configuration: its full sdkconfig when it has one (a device build's), else its
    defaults files over ESP-IDF's defaults."""
    if sdkconfig_path and os.path.isfile(sdkconfig_path):
        with open(sdkconfig_path) as source:
            return complete_sdkconfig(dict(IDF_DEFAULTS, **parse_sdkconfig(source.read())))
    layers = []
    for path in defaults_paths:
        if os.path.isfile(path):
            with open(path) as source:
                layers.append(parse_sdkconfig(source.read()))
    return merge_sdkconfig(layers)


def write_if_changed(path, text):
    if os.path.isfile(path):
        with open(path) as existing:
            if existing.read() == text:
                return
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w") as out:
        out.write(text)


# ---- what a failed build means -------------------------------------------------------------------------

# Headers of ESP-IDF (or of the chip) the web target does not have: including one does not follow the HAL.
_IDF_HEADER_DIRS = {"driver", "soc", "hal", "rom", "esp_private", "freertos", "esp32s3", "esp32", "xtensa", "lwip", "nimble",
                    "host", "esp_rom", "bootloader_support", "esp_wifi", "esp_netif", "ulp", "riscv", "usb", "tinyusb",
                    "mbedtls", "psa", "esp_flash", "spi_flash", "esp_partition", "esp_adc", "esp_eth", "esp_ble", "esp_bt"}
_IDF_HEADER_NAMES = re.compile(r"^(esp_|esp32|nvs|sdmmc|sdspi|tusb|tinyusb|wifi|bt|ble_|lwip|xtensa|xt_|ulp|spi_flash|bootloader|"
                               r"rtc_|ets_|spiffs|wear_levelling|sdkconfig|esp-|class/|device/)")
# ESP-IDF's C library (newlib) has headers the web target's (musl) does not.
_IDF_NEWLIB_HEADERS = {"sys/lock.h", "sys/reent.h", "sys/_types.h", "sys/features.h", "newlib.h", "_ansi.h", "sys/config.h"}
# Names of ESP-IDF's and FreeRTOS's API: an undeclared one is a function the web target lacks.
_IDF_NAMES = re.compile(r"^(esp_|gpio_|i2s_|i2c_|ledc_|rmt_|spi_|uart_|adc_|dac_|touch_|sdmmc_|sdspi_|nvs_|heap_caps_|gptimer_|"
                        r"mcpwm_|pcnt_|twai_|wifi_|ble_|rtc_|ets_|tusb_|tud_|ulp_|sdm_|temperature_sensor_|usb_serial_jtag_|"
                        r"(x|v|ul|ux|pc|pv)(Task|Queue|Semaphore|Timer|EventGroup|StreamBuffer|MessageBuffer|Ringbuffer|Port)|"
                        r"portMUX|portENTER|portEXIT|portYIELD|xPortGet|esp32)")

_UNDEFINED = re.compile(r"undefined symbol: (.+?)(?: \(referenced by .*\))?\s*$")
_UNAVAILABLE = re.compile(r"'([^']+)' is unavailable: (not provided by the CardenzaHAL web target|set up by CardenzaHAL)")
_MISSING_HEADER = re.compile(r"fatal error: '([^']+)' file not found")
_UNDECLARED = re.compile(r"(?:use of undeclared identifier|call to undeclared function|implicit declaration of function) '([^']+)'")
_UNSUPPORTED_SOURCE = re.compile(r"cardenza-web: unsupported-source: (\S+)")
_ERROR = re.compile(r"(^|\s)(error|fatal error):\s|^CMake Error")
# What follows from an error already counted: the compiler driver saying the linker failed.
_CONSEQUENCE = re.compile(r"^(em\+\+|emcc|clang\+*): error: .*(wasm-ld|failed \(returned \d+\))")


def idf_header(header):
    parts = header.split("/")
    return parts[0] in _IDF_HEADER_DIRS or header in _IDF_NEWLIB_HEADERS or bool(_IDF_HEADER_NAMES.match(header))


def classify_log(text):
    """A build log as (unsupported, errors): the unsupported items as ("unsupported" | "unsupported-header" |
    "unsupported-source", name) in the order met, without repeats, and the firmware's own error lines."""
    unsupported = []
    errors = []

    def add(kind, name):
        if (kind, name) not in unsupported:
            unsupported.append((kind, name))

    for line in text.splitlines():
        match = _UNDEFINED.search(line)
        if match:
            add("unsupported", match.group(1).strip())
            continue
        match = _UNAVAILABLE.search(line)
        if match:
            add("unsupported", match.group(1))
            continue
        match = _MISSING_HEADER.search(line)
        if match:
            if idf_header(match.group(1)):
                add("unsupported-header", match.group(1))
            else:
                errors.append(line.strip())
            continue
        match = _UNDECLARED.search(line)
        if match:
            if _IDF_NAMES.match(match.group(1)):
                add("unsupported", match.group(1))
            else:
                errors.append(line.strip())
            continue
        match = _UNSUPPORTED_SOURCE.search(line)
        if match:
            add("unsupported-source", match.group(1))
            continue
        if _ERROR.search(line) and not _CONSEQUENCE.search(line):
            errors.append(line.strip())
    return unsupported, errors


# ---- toolchain and project ------------------------------------------------------------------------------

def _version_tuple(text):
    match = re.search(r"(\d+)\.(\d+)(?:\.(\d+))?", text)
    return tuple(int(part or 0) for part in match.groups()) if match else (0, 0, 0)


def find_toolchain(environ=None):
    """emcmake, cmake (3.20 or later) and a generator; raises ToolchainMissing with the hint."""
    environ = dict(os.environ if environ is None else environ)
    path = environ.get("PATH", "")
    emcmake = shutil.which("emcmake", path=path)
    env_updates = {}
    if emcmake is None:
        home = environ.get("HOME") or os.path.expanduser("~")
        roots = [environ.get("EMSDK"), os.path.join(home, "emsdk")]
        for root in roots:
            if not root:
                continue
            candidate = os.path.join(root, "upstream", "emscripten", "emcmake")
            if os.path.isfile(candidate):
                emcmake = candidate
                env_updates["EMSDK"] = root
                config = os.path.join(root, ".emscripten")
                if os.path.isfile(config) and "EM_CONFIG" not in environ:
                    env_updates["EM_CONFIG"] = config
                # emcc runs on the node and python emsdk installed beside it.
                extra = [os.path.join(root, "upstream", "emscripten")]
                for tool in ("node", "python"):
                    tool_root = os.path.join(root, tool)
                    if os.path.isdir(tool_root):
                        for version in sorted(os.listdir(tool_root), reverse=True):
                            for sub in ("bin", ""):
                                folder = os.path.join(tool_root, version, sub)
                                if os.path.isdir(folder):
                                    extra.append(folder)
                env_updates["PATH"] = os.pathsep.join(extra + [path])
                break
    if emcmake is None:
        raise ToolchainMissing(EMSCRIPTEN_HINT)
    cmake = environ.get("CMAKE") or shutil.which("cmake", path=path)
    if cmake is None or not os.path.exists(cmake):
        raise ToolchainMissing(CMAKE_HINT)
    try:
        version = subprocess.run([cmake, "--version"], capture_output=True, text=True).stdout
    except OSError:
        raise ToolchainMissing(CMAKE_HINT)
    if _version_tuple(version) < (3, 20, 0):
        raise ToolchainMissing("CMake %s is too old.\n%s" % (".".join(map(str, _version_tuple(version))), CMAKE_HINT))
    ninja = shutil.which("ninja", path=path)
    generator = ["-G", "Ninja"] if ninja else []
    if not ninja and not shutil.which("make", path=path):
        raise ToolchainMissing("Neither ninja nor make was found: install ninja (https://ninja-build.org, 'pip install ninja').")
    emcc = os.path.join(os.path.dirname(emcmake), "emcc")
    return {"emcmake": emcmake, "emcc": emcc, "cmake": cmake, "generator": generator, "env": env_updates}


def emscripten_version(toolchain):
    env = dict(os.environ, **toolchain["env"])
    try:
        out = subprocess.run([toolchain["emcc"], "--version"], capture_output=True, text=True, env=env).stdout
    except OSError:
        return None
    match = re.search(r"\b(\d+\.\d+\.\d+)\b", out)
    return match.group(1) if match else None


def is_idf_project(project_dir):
    """A folder whose CMakeLists.txt includes ESP-IDF's project.cmake."""
    lists = os.path.join(project_dir, "CMakeLists.txt")
    if not os.path.isfile(lists):
        return False
    with open(lists, errors="replace") as source:
        text = source.read()
    return "project.cmake" in text and "IDF_PATH" in text


_SKIP_DIRS = {".git", "node_modules", "managed_components", ".venv", "__pycache__", "build-web"}


def find_project_hal(project_dir, depth=4):
    """The CardenzaHAL a firmware builds with: the folder with cardenzaHAL.h in the project (its
    components, a submodule) or above it (the HAL's own examples)."""
    project_dir = os.path.abspath(project_dir)
    queue = [(project_dir, 0)]
    while queue:
        folder, level = queue.pop(0)
        if os.path.isfile(os.path.join(folder, "cardenzaHAL.h")):
            return folder
        if level >= depth:
            continue
        try:
            entries = sorted(os.listdir(folder))
        except OSError:
            continue
        for entry in entries:
            child = os.path.join(folder, entry)
            if os.path.isdir(child) and entry not in _SKIP_DIRS and not entry.startswith("build"):
                queue.append((child, level + 1))
    parent = os.path.dirname(project_dir)
    for _ in range(3):
        if os.path.isfile(os.path.join(parent, "cardenzaHAL.h")):
            return parent
        parent = os.path.dirname(parent)
    return None


def has_web_target(hal_dir):
    return bool(hal_dir) and os.path.isfile(os.path.join(hal_dir, "web", "idf", "tools", "cmake", "project.cmake"))


def choose_hal(project_dir, explicit=None, own=HAL_DIR):
    """(the HAL to build with, a warning or None): --hal, else the firmware's own when it has the web
    target, else this tool's."""
    if explicit:
        explicit = os.path.abspath(explicit)
        if not has_web_target(explicit):
            raise ValueError("%s has no web target (web/idf)" % explicit)
        return explicit, None
    project_hal = find_project_hal(project_dir)
    if project_hal and has_web_target(project_hal):
        return project_hal, None
    if project_hal:
        return own, ("the firmware's CardenzaHAL (%s, %s) has no web target: building against CardenzaHAL %s (%s)"
                     % (project_hal, git_commit(project_hal), own, git_commit(own)))
    return own, None


def git_commit(folder):
    result = subprocess.run(["git", "-C", folder, "rev-parse", "HEAD"], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 and result.stdout.strip() else "unknown"


def hal_identity(hal_dir):
    """{"version", "commit", "dirty"} of a HAL checkout."""
    version = None
    version_file = os.path.join(hal_dir, "version.txt")
    if os.path.isfile(version_file):
        with open(version_file) as source:
            version = source.readline().strip() or None
    status = subprocess.run(["git", "-C", hal_dir, "status", "--porcelain"], capture_output=True, text=True)
    dirty = status.returncode == 0 and bool(status.stdout.strip())
    return {"version": version, "commit": git_commit(hal_dir), "dirty": dirty}


# ---- the page --------------------------------------------------------------------------------------------

PLACEHOLDER_PAGE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>@CARDENZA_TITLE@</title>
<style>html,body{margin:0;height:100%;background:#000}canvas{width:100%;height:100%;object-fit:contain;image-rendering:pixelated}</style>
</head><body><canvas id="screen" width="240" height="135"></canvas>
<script src="@CARDENZA_MODULE@"></script>
<script>
// The web target's page (web/page) was not there when this was built: this one only shows the screen.
createCardenzaFirmware({ cardenzaConfig: {}, cardenzaBooted() { parent.postMessage({ cardenza_sim: 'ready' }, '*'); } })
  .then((M) => {
    const context = document.getElementById('screen').getContext('2d');
    const image = context.createImageData(240, 135);
    const pump = () => {
      M._cardenza_web_run(8);
      const frame = M._cardenza_web_display() >> 1;
      const pixels = M.HEAPU16.subarray(frame + 6, frame + 6 + 240 * 135);
      for (let i = 0; i < pixels.length; ++i) {
        const p = pixels[i];
        image.data.set([(p >> 11) * 255 / 31, ((p >> 5) & 63) * 255 / 63, (p & 31) * 255 / 31, 255], i * 4);
      }
      context.putImageData(image, 0, 0);
      const delay = M._cardenza_web_next_delay();
      setTimeout(pump, delay < 0 ? 16 : Math.min(delay, 16));
    };
    pump();
  });
</script></body></html>
"""

_PAGE_SKIP = {"index.html", "keymap.json", "README.md", "README.zh-CN.md"}


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(1 << 16), b""):
            digest.update(block)
    return digest.hexdigest()


def load_keys(page_dir):
    """The page's key map for sim.json: [[device key, computer keys], ...] (web/page/keymap.json)."""
    path = os.path.join(page_dir, "keymap.json")
    if not os.path.isfile(path):
        return []
    with open(path) as source:
        data = json.load(source)
    if isinstance(data, dict):
        data = data.get("keys", [])
    return data


def render_page(template, name, version, module):
    return (template.replace("@CARDENZA_MODULE@", html.escape(module, quote=True))
            .replace("@CARDENZA_TITLE@", html.escape(name))
            .replace("@CARDENZA_VERSION@", html.escape(version)))


def assemble(project, site_dir=None, toolchain_version=None):
    """Puts the page beside the module and writes sim.json. project: cardenza_web_project.json's content."""
    name = project["name"]
    version = project.get("version") or "1"
    hal_dir = project.get("hal_dir") or HAL_DIR
    site_dir = site_dir or project["site_dir"]
    page_dir = os.path.join(hal_dir, "web", "page")
    module = project.get("module") or name + ".js"
    files = []
    template_path = os.path.join(page_dir, "index.html")
    if os.path.isfile(template_path):
        with open(template_path) as source:
            template = source.read()
    else:
        template = PLACEHOLDER_PAGE
    write_if_changed(os.path.join(site_dir, "index.html"), render_page(template, name, version, module))
    files.append("index.html")
    if os.path.isdir(page_dir):
        for entry in sorted(os.listdir(page_dir)):
            source = os.path.join(page_dir, entry)
            if entry in _PAGE_SKIP or not os.path.isfile(source) or entry.startswith("."):
                continue
            shutil.copyfile(source, os.path.join(site_dir, entry))
            files.append(entry)
    base = os.path.splitext(module)[0]
    for entry in sorted(os.listdir(site_dir)):
        if entry.startswith(base + ".") and entry not in files:
            files.append(entry)
    identity = hal_identity(hal_dir)
    if identity["version"] is None and project.get("hal_version") and "+" in project["hal_version"]:
        identity["version"] = project["hal_version"].split("+")[0]
    manifest = {
        "format": 1,
        "target": "cardenza",
        "entry": "index.html",
        "firmware": {"name": name, "version": version},
        "hal": identity,
        "toolchain": {"emscripten": toolchain_version or project.get("emscripten") or None},
        "headers": {},
        "keys": load_keys(page_dir),
        "files": [{"path": entry, "size": os.path.getsize(os.path.join(site_dir, entry)),
                   "sha256": sha256_file(os.path.join(site_dir, entry))} for entry in files],
    }
    write_if_changed(os.path.join(site_dir, "sim.json"), json.dumps(manifest, indent=2) + "\n")
    return manifest


# ---- build ------------------------------------------------------------------------------------------------

def run_logged(command, env, verbose):
    """Runs a command; returns (exit code, combined output). With verbose the output goes to stderr too."""
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, text=True, errors="replace")
    lines = []
    for line in process.stdout:
        lines.append(line)
        if verbose:
            sys.stderr.write(line)
    return process.wait(), "".join(lines)


def report_failure(log):
    unsupported, errors = classify_log(log)
    if unsupported:
        sys.stderr.write("cardenza_web: this firmware does not follow the HAL: it uses what the web target does not provide\n")
        for kind, name in unsupported:
            sys.stderr.write("%s: %s\n" % (kind, name))
        if errors:
            sys.stderr.write("cardenza_web: other errors:\n%s\n" % "\n".join(errors[:20]))
        return EXIT_UNSUPPORTED
    tail = log.splitlines()[-60:]
    sys.stderr.write("\n".join(tail) + "\ncardenza_web: the firmware did not build\n")
    return EXIT_BUILD_FAILED


def build(arguments):
    project_dir = os.path.abspath(arguments.project)
    if not os.path.isdir(project_dir) or not is_idf_project(project_dir):
        sys.stderr.write("cardenza_web: %s is not an ESP-IDF firmware project (its CMakeLists.txt includes "
                         "$ENV{IDF_PATH}/tools/cmake/project.cmake)\n" % project_dir)
        return EXIT_USAGE
    try:
        toolchain = find_toolchain()
    except ToolchainMissing as missing:
        sys.stderr.write("cardenza_web: %s\n" % missing)
        return EXIT_TOOLCHAIN
    try:
        hal_dir, warning = choose_hal(project_dir, arguments.hal)
    except ValueError as error:
        sys.stderr.write("cardenza_web: %s\n" % error)
        return EXIT_USAGE
    if warning:
        sys.stderr.write("cardenza_web: warning: %s\n" % warning)
    build_dir = os.path.abspath(arguments.build_dir or os.path.join(project_dir, "build-web", "cmake"))
    site_dir = os.path.abspath(arguments.out or os.path.join(project_dir, "build-web", "site"))
    os.makedirs(site_dir, exist_ok=True)
    env = dict(os.environ, **toolchain["env"])
    env["IDF_PATH"] = os.path.join(hal_dir, "web", "idf")
    build_type = "Debug" if arguments.debug else "Release"
    configure = [toolchain["emcmake"], toolchain["cmake"], "-S", project_dir, "-B", build_dir] + toolchain["generator"] + [
        "-DCMAKE_BUILD_TYPE=" + build_type, "-DCARDENZA_WEB_SITE_DIR=" + site_dir, "-DPython3_EXECUTABLE=" + sys.executable]
    code, log = run_logged(configure, env, arguments.verbose)
    if code != 0:
        return report_failure(log)
    command = [toolchain["cmake"], "--build", build_dir]
    if arguments.jobs:
        command += ["-j", str(arguments.jobs)]
    # Keep compiling after a failure, so every unsupported call and header is listed, not the first one.
    command += ["--", "-k", "0"] if toolchain["generator"] else ["--", "-k"]
    code, build_log = run_logged(command, env, arguments.verbose)
    if code != 0:
        return report_failure(log + build_log)
    project_json = os.path.join(build_dir, "cardenza_web_project.json")
    with open(project_json) as source:
        project = json.load(source)
    # The POST_BUILD step wrote it; the toolchain's version comes from here.
    manifest = assemble(project, site_dir, emscripten_version(toolchain))
    hal_component = project.get("hal_component_dir")
    if hal_component and not os.path.samefile(hal_component, hal_dir) and not warning and not has_web_target(hal_component):
        sys.stderr.write("cardenza_web: warning: the firmware's CardenzaHAL (%s) has no web target: built against %s\n"
                         % (hal_component, hal_dir))
    print("cardenza_web: built %s %s with CardenzaHAL %s" % (manifest["firmware"]["name"], manifest["firmware"]["version"],
                                                            manifest["hal"]["commit"]))
    print(site_dir)
    for entry in manifest["files"]:
        print("  %-24s %9d" % (entry["path"], entry["size"]))
    return EXIT_BUILT


# ---- serve ------------------------------------------------------------------------------------------------

SANDBOX_PAGE = """<!doctype html>
<html><head><meta charset="utf-8"><title>sandboxed simulator</title>
<style>html,body{margin:0;height:100%%;background:#111;color:#ccc;font:13px sans-serif}
iframe{border:0;width:100%%;height:calc(100%% - 24px)}#log{height:24px;line-height:24px;padding:0 8px}</style></head>
<body><div id="log">waiting for {"cardenza_sim":"ready"}...</div>
<iframe src="%s" sandbox="allow-scripts" allow="autoplay"></iframe>
<script>
addEventListener('message', (event) => {
  const data = event.data || {};
  if (data.cardenza_sim) document.getElementById('log').textContent = 'from the frame: ' + JSON.stringify(data).slice(0, 300);
});
</script></body></html>
"""


def make_handler(directory, sandbox_target):
    class Handler(http.server.SimpleHTTPRequestHandler):
        extensions_map = dict(http.server.SimpleHTTPRequestHandler.extensions_map,
                              **{".wasm": "application/wasm", ".js": "text/javascript", ".mjs": "text/javascript",
                                 ".json": "application/json"})

        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=directory, **kwargs)

        def end_headers(self):
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Cache-Control", "no-store")
            super().end_headers()

        def do_GET(self):
            if self.path.split("?")[0] == "/__sandbox.html":
                body = (SANDBOX_PAGE % html.escape(sandbox_target, quote=True)).encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            super().do_GET()

        def log_message(self, format, *args):
            pass

    return Handler


def serve(arguments):
    directory = os.path.abspath(arguments.site)
    if not os.path.isfile(os.path.join(directory, "index.html")):
        sys.stderr.write("cardenza_web: %s has no index.html\n" % directory)
        return EXIT_USAGE
    host = arguments.host
    site = http.server.ThreadingHTTPServer((host, arguments.port), make_handler(directory, "/index.html"))
    port = site.server_address[1]
    target = "http://%s:%d/index.html" % ("localhost" if host in ("", "0.0.0.0", "127.0.0.1") else host, port)
    frame = http.server.ThreadingHTTPServer((host, port + 1), make_handler(directory, target))
    threading.Thread(target=frame.serve_forever, daemon=True).start()
    print("cardenza_web: %s" % target)
    print("cardenza_web: sandboxed, from another origin: http://localhost:%d/__sandbox.html" % (port + 1))
    sys.stdout.flush()
    try:
        site.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


# ---- the shim's helpers -----------------------------------------------------------------------------------

def sdkconfig_command(arguments):
    values = make_sdkconfig(arguments.sdkconfig, arguments.defaults or [])
    write_if_changed(os.path.join(arguments.out, "sdkconfig.h"), render_sdkconfig_h(values))
    write_if_changed(os.path.join(arguments.out, "sdkconfig.cmake"), render_sdkconfig_cmake(values))
    return 0


def assemble_command(arguments):
    with open(arguments.project_json) as source:
        project = json.load(source)
    assemble(project, arguments.site)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(prog="cardenza_web.py", description="The CardenzaHAL web target: build and serve the "
                                     "simulator version of a firmware.")
    commands = parser.add_subparsers(dest="command", required=True)
    build_parser = commands.add_parser("build", help="build a firmware project for the web")
    build_parser.add_argument("project", help="the firmware's ESP-IDF project folder")
    build_parser.add_argument("--out", help="the page's folder (default <project>/build-web/site)")
    build_parser.add_argument("--build-dir", help="CMake's build folder (default <project>/build-web/cmake)")
    build_parser.add_argument("--hal", help="build against this CardenzaHAL (default: the firmware's own, else this one)")
    build_parser.add_argument("--debug", action="store_true", help="-O1 -g, with Emscripten's assertions")
    build_parser.add_argument("--jobs", type=int, help="parallel compile jobs")
    build_parser.add_argument("--verbose", action="store_true", help="show the build as it runs")
    serve_parser = commands.add_parser("serve", help="serve a built site locally")
    serve_parser.add_argument("site")
    serve_parser.add_argument("--port", type=int, default=8000)
    serve_parser.add_argument("--host", default="127.0.0.1")
    config_parser = commands.add_parser("sdkconfig", help=argparse.SUPPRESS)
    config_parser.add_argument("--out", required=True)
    config_parser.add_argument("--sdkconfig")
    config_parser.add_argument("--defaults", action="append")
    assemble_parser = commands.add_parser("assemble", help=argparse.SUPPRESS)
    assemble_parser.add_argument("--project-json", required=True)
    assemble_parser.add_argument("--site")
    arguments = parser.parse_args(argv)
    return {"build": build, "serve": serve, "sdkconfig": sdkconfig_command, "assemble": assemble_command}[arguments.command](arguments)


if __name__ == "__main__":
    sys.exit(main())
