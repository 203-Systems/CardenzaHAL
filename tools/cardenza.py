#!/usr/bin/env python3
"""The preferred way to build and try a firmware on CardenzaHAL: it checks what the project needs first
(ESP-IDF 5.3, git, a clean HAL submodule, 203c, the 203 community's CLI), then runs the step. Linux, macOS
and Windows; Python 3.8 or later, standard library only.

usage: python3 components/CardenzaHAL/tools/cardenza.py <command> [options]

  doctor          everything the project needs on this computer, and how to install what is missing; whether
                  203c or the HAL submodule has an update (203c updates)
  build [--web]   the checks, then the device build (idf.py build): build/<project>.bin and how much of the
                  203 Launcher's room it takes; --web also the browser version (build-web/site)
  web             the browser version only (tools/cardenza_web.py build .)
  flash [...]     build/<project>.bin onto the Cardenza on the USB cable, through its 203 Launcher, which
                  stays (203c flash --bin ...; never idf.py flash: it erases the Launcher)
  log [...]       the device's log over USB (203c log: a running firmware's console, or the Launcher's)
  help            this text

  -C DIR          the project (default: the folder with 203.json here or above)
  --json          doctor: the checks as JSON
  --no-cli-check  build without 203c (CI only: the publish step installs it)

ESP-IDF does not have to be activated: an ESP-IDF 5.3 found in IDF_PATH, ~/esp or C:\\Espressif is
exported for the build. What the rules say about every step: docs/FIRMWARE-PROJECT.md.
"""

import glob
import json
import os
import re
import shutil
import subprocess
import sys

HAL = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RULES = "https://github.com/203-Systems/CardenzaHAL/blob/main/docs/FIRMWARE-PROJECT.md"
INSTALL_203C = "npm install -g 203c && 203c skill install"
IDF_WANTED = (5, 3)
IDF_TESTED = "5.3.5"
CLI_WANTED = (3, 5, 0)   # 203c new, log, sync, link, skill install
CLI_UPDATES = (3, 5, 1)  # 203c updates (a newer 203c, the HAL submodule behind its main): the release after 3.5.0
APP_ROOM = 0x690000      # the 203 Launcher's room for one app on every Cardenza (0x170000 to 8 MB: boards
                         # flashed with Launcher 0.3.3 or earlier declare 8 MB in their bootloader; the chip has 16 MB)
LOCAL_FLASH = "https://cardenza.203.io/community/firmware (Flash a local file / 烧录本地固件, in Chrome or Edge)"
WINDOWS = os.name == "nt"
PY = "python" if WINDOWS else "python3"   # what to type: Windows has python (and py), rarely python3


def version_tuple(text):
    return tuple(int(part) for part in re.findall(r"\d+", text or "")[:3])


def find_project(start):
    """The project's folder: the nearest one with 203.json, else `start` when it is an ESP-IDF project."""
    folder = os.path.abspath(start)
    while True:
        if os.path.isfile(os.path.join(folder, "203.json")) and os.path.isfile(os.path.join(folder, "CMakeLists.txt")):
            return folder
        parent = os.path.dirname(folder)
        if parent == folder:
            break
        folder = parent
    return os.path.abspath(start) if os.path.isfile(os.path.join(start, "CMakeLists.txt")) else None


def project_name(folder):
    try:
        with open(os.path.join(folder, "CMakeLists.txt"), encoding="utf-8") as cmake:
            found = re.search(r"^\s*project\(\s*([A-Za-z0-9_.+-]+)", cmake.read(), re.M)
    except OSError:
        return None
    return found.group(1) if found else None


def built_bin(folder):
    """build/<project>.bin as the build names it (project_description.json), else from CMake's project()."""
    try:
        with open(os.path.join(folder, "build", "project_description.json"), encoding="utf-8") as description:
            app = json.load(description).get("app_bin")
        if app:
            return os.path.join(folder, "build", app)
    except (OSError, ValueError):
        pass
    name = project_name(folder)
    return os.path.join(folder, "build", name + ".bin") if name else None


def idf_version(path):
    try:
        with open(os.path.join(path, "tools", "cmake", "version.cmake"), encoding="utf-8") as cmake:
            text = cmake.read()
    except OSError:
        return None
    parts = [re.search(r"IDF_VERSION_%s\s+(\d+)" % key, text) for key in ("MAJOR", "MINOR", "PATCH")]
    return ".".join(p.group(1) for p in parts) if all(parts) else None


def find_idf(env=os.environ, home=os.path.expanduser("~")):
    """(path, version, activated): the ESP-IDF to build with, preferring an activated one, then 5.3.5."""
    activated = env.get("IDF_PATH") and shutil.which("idf.py", path=env.get("PATH"))
    candidates = [env["IDF_PATH"]] if env.get("IDF_PATH") else []
    for pattern in ("esp/esp-idf*", "esp/idf*", ".espressif/frameworks/esp-idf*"):
        candidates += sorted(glob.glob(os.path.join(home, pattern)), reverse=True)
    if WINDOWS:
        candidates += sorted(glob.glob("C:/Espressif/frameworks/esp-idf*"), reverse=True)
    best = None
    for path in candidates:
        version = idf_version(path)
        if not version or version_tuple(version)[:2] != IDF_WANTED:
            if path == env.get("IDF_PATH") and version:
                return path, version, bool(activated)   # the one activated, whatever it is: doctor says so
            continue
        if path == env.get("IDF_PATH"):
            return path, version, bool(activated)
        if best is None or version == IDF_TESTED:
            best = (path, version, False)
            if version == IDF_TESTED:
                break
    return best or (None, None, False)


def find_203c(env=os.environ, home=os.path.expanduser("~")):
    """The command that runs 203c ([203c] or [node, .../203c.mjs]), or None."""
    found = shutil.which("203c", path=env.get("PATH"))
    if found:
        return [found]
    skills = env.get("CLAUDE_SKILLS_DIR") or os.path.join(home, ".claude", "skills")
    script = os.path.join(skills, "203-community", "scripts", "203c.mjs")
    node = shutil.which("node", path=env.get("PATH"))
    return [node, script] if node and os.path.isfile(script) else None


def signed_in(env=os.environ, home=os.path.expanduser("~")):
    if env.get("COMMUNITY_TOKEN"):
        return True
    base = env.get("XDG_CONFIG_HOME") or os.path.join(home, ".config")
    try:
        with open(os.path.join(base, "203c", "token"), encoding="utf-8") as token:
            return bool(token.read().strip())
    except OSError:
        return False


def run_quiet(command, timeout=60, **kwargs):
    try:
        return subprocess.run(command, capture_output=True, text=True, timeout=timeout, **kwargs)
    except (OSError, subprocess.TimeoutExpired):
        return None


def updates(cli, project):
    """203c's update notices, asked now (`203c updates --json`): [(status, detail)], or [] when it does not answer.
    The HAL's own check (check_submodule.py) is the "hal" line already: not said twice."""
    said = run_quiet(cli + ["updates", "--json"] + (["--project", project] if project else []), timeout=15)
    try:
        answer = json.loads(said.stdout) if said and said.returncode == 0 else None
    except ValueError:
        answer = None
    if not isinstance(answer, dict):
        return []
    hal = answer.get("hal") or {}
    notices = [n for n in answer.get("notices") or [] if not (hal.get("local") and hal["local"] in n)]
    if notices:
        return [("warn", notice) for notice in notices]
    cli_said = answer.get("cli") or {}
    if not cli_said.get("latest"):
        return [("skip", "npm did not answer (offline, or blocked here): not checked")]
    detail = "203c %s is the newest" % cli_said.get("current")
    if hal.get("status") in ("identical", "ahead"):
        detail += "; CardenzaHAL (%s) is at its main" % hal.get("path")
    return [("ok", detail)]


def checks(project, need_cli=True, updates_too=False):
    """Each check: {name, status: ok | missing | warn | skip, detail, fix}. missing stops a build. updates_too (doctor):
    203c's update notices too (a newer 203c, the HAL submodule behind its main), asked now."""
    out = []

    def add(name, status, detail, fix=None):
        out.append({"name": name, "status": status, "detail": detail, **({"fix": fix} if fix and status != "ok" else {})})

    add("python", "ok" if sys.version_info >= (3, 8) else "missing", "Python %d.%d" % sys.version_info[:2], "Python 3.8 or later")
    add("git", "ok" if shutil.which("git") else "missing", "git" if shutil.which("git") else "not found", "https://git-scm.com/downloads")
    if project:
        add("project", "ok", "%s (%s)" % (project, project_name(project) or "no project() in CMakeLists.txt"))
        for name in ("203.json", "version.txt", "CHANGELOG.md", "sdkconfig.defaults"):
            if not os.path.isfile(os.path.join(project, name)):
                add(name, "warn", "missing", "the template has it (203c new, or %s#2-the-project-files-and-layout)" % RULES)
    else:
        add("project", "missing", "no ESP-IDF project with 203.json here or above", "203c new \"My firmware\" (or -C DIR)")

    path, version, activated = find_idf()
    if not path:
        add("esp-idf", "missing", "ESP-IDF %s not found (IDF_PATH, ~/esp, C:\\Espressif)" % IDF_TESTED, "%s#esp-idf-535" % RULES)
    elif version_tuple(version)[:2] != IDF_WANTED:
        add("esp-idf", "missing", "ESP-IDF %s at %s; the firmware builds with 5.3 (%s)" % (version, path, IDF_TESTED), "%s#esp-idf-535" % RULES)
    else:
        add("esp-idf", "ok" if version == IDF_TESTED else "warn", "ESP-IDF %s at %s%s" % (version, path, "" if activated else " (exported for the build)"),
            None if version == IDF_TESTED else "the official firmware and the board defaults are checked with %s" % IDF_TESTED)

    inside = project and os.path.commonpath([os.path.abspath(project), HAL]) == HAL
    if inside:
        add("hal", "ok", "%s (the HAL's own repository: an example built in place)" % HAL)
    elif os.path.exists(os.path.join(HAL, ".git")):
        script = os.path.join(HAL, "tools", "check_submodule.py")
        result = run_quiet([sys.executable, script])
        if result is not None and result.returncode != 0:
            result = run_quiet([sys.executable, script, "--fetch"])
        clean = result is not None and result.returncode == 0
        add("hal", "ok" if clean else "missing", HAL if clean else (result.stderr.strip() if result else "check_submodule.py did not run"),
            None if clean else "restore the HAL (git -C %s checkout -- .), change it upstream instead" % HAL)
    else:
        add("hal", "ok", "%s (not a git checkout: not checked)" % HAL)

    cli = find_203c()
    if not cli:
        add("203c", "missing" if need_cli else "warn", "203c, the 203 community's CLI, is not installed",
            "%s   (Node.js 18 or later: https://nodejs.org)" % INSTALL_203C)
    else:
        said = run_quiet(cli + ["--version"])
        version = said.stdout.strip() if said and said.returncode == 0 else "?"
        old = version_tuple(version) < CLI_WANTED
        add("203c", "warn" if old else "ok", "203c %s (%s)" % (version, " ".join(cli)),
            "update it: %s" % INSTALL_203C if old else None)
        add("sign-in", "ok" if signed_in() else "warn", "signed in" if signed_in() else "not signed in",
            None if signed_in() else "203c login (once on this computer: the browser asks you to approve; publishing needs it)")
        if updates_too and version_tuple(version) >= CLI_UPDATES:
            for status, detail in updates(cli, project):
                add("updates", status, detail)

    emcc = shutil.which("emcc") or (os.environ.get("EMSDK") and os.path.isdir(os.environ["EMSDK"])) or os.path.isdir(os.path.expanduser("~/emsdk"))
    add("emscripten", "ok" if emcc else "skip", "found" if emcc else "not found (only for the browser version)",
        None if emcc else "%s#emscripten-the-simulator-version" % RULES)
    return out


def print_checks(results):
    marks = {"ok": "[ok]  ", "missing": "[MISSING]", "warn": "[warn]", "skip": "[--]  "}
    for check in results:
        print("%-9s %-12s %s" % (marks[check["status"]], check["name"], check["detail"]))
        if check.get("fix") and check["status"] != "ok":
            print("%-22s -> %s" % ("", check["fix"]))


def in_idf(command, project):
    """Runs `command` (a list) in the project with ESP-IDF's environment (idf.py, its CMake and ninja): the shell's when it
    is activated, else ESP-IDF's export script first (an agent's shell starts fresh every time)."""
    path, _version, activated = find_idf()
    if activated or not path:
        return subprocess.run(subprocess.list2cmdline(command) if WINDOWS else command, shell=WINDOWS, cwd=project).returncode
    if WINDOWS:
        return subprocess.run('call "%s" >nul && %s' % (os.path.join(path, "export.bat"), subprocess.list2cmdline(command)), shell=True, cwd=project).returncode
    return subprocess.run(["bash", "-c", '. "$0/export.sh" > /dev/null && exec "$@"', path] + command, cwd=project).returncode


def need(project, results):
    missing = [check for check in results if check["status"] == "missing"]
    if missing:
        print_checks(missing)
        sys.stderr.write("cardenza: install or fix the above first (%s %s doctor shows everything)\n" % (PY, os.path.relpath(__file__, project or ".")))
        return False
    return True


def build(project, options):
    results = checks(project, need_cli="--no-cli-check" not in options)
    if not need(project, results):
        return 1
    for check in results:
        if check["status"] == "warn" and check["name"] in ("203c", "sign-in", "esp-idf"):
            print("cardenza: %s: %s -> %s" % (check["name"], check["detail"], check.get("fix", "")))
    code = in_idf(["idf.py", "build"], project)
    if code != 0:
        sys.stderr.write("cardenza: the build failed (above). Fix every error and warning of your own code.\n")
        return code
    image = built_bin(project)
    size = os.path.getsize(image) if image and os.path.isfile(image) else 0
    rounded = (size + 0xFFFF) & ~0xFFFF
    print("cardenza: %s, %d KB: %d%% of the 203 Launcher's room for an app (%.2f MB)"
          % (os.path.relpath(image, project), size // 1024, rounded * 100 // APP_ROOM, APP_ROOM / 1048576))
    if rounded > APP_ROOM:
        sys.stderr.write("cardenza: too large for the Launcher: it installs at most 0x%X bytes\n" % APP_ROOM)
        return 1
    if "--web" in options and web(project) != 0:
        return 1
    rel = os.path.relpath(image, project)
    print("next: 203c flash --bin %s   onto the Cardenza over USB, through its 203 Launcher (not idf.py flash: that erases the Launcher)" % rel)
    print("      203c log                 what it prints")
    print("      203c publish --dry-run   what the community will check")
    return 0


def web(project):
    # The web build needs CMake and ninja: ESP-IDF's are fine, so it runs in ESP-IDF's environment when there is no CMake.
    command = ["python3" if not WINDOWS else "python", os.path.join(HAL, "tools", "cardenza_web.py"), "build", project]
    if shutil.which("cmake") or not find_idf()[0]:
        return subprocess.run([sys.executable] + command[1:], cwd=project).returncode
    return in_idf(command, project)


def through_203c(arguments, project):
    cli = find_203c()
    if not cli:
        sys.stderr.write("cardenza: 203c, the 203 community's CLI, is not installed: %s\n" % INSTALL_203C)
        return 1
    code = subprocess.run(cli + arguments, cwd=project or None).returncode
    if code != 0 and WINDOWS and arguments[0] in ("flash", "log"):
        sys.stderr.write("cardenza: on Windows the USB cable is used from the browser: %s; the log: "
                         "pip install hidapi, then python %s console 60\n" % (LOCAL_FLASH, os.path.join(HAL, "tools", "cardenza_usb.py")))
    return code


def flash(project, arguments):
    image = built_bin(project) if project else None
    if not image or not os.path.isfile(image):
        sys.stderr.write("cardenza: no build yet: %s %s build\n" % (PY, os.path.relpath(__file__, project or ".")))
        return 1
    return through_203c(["flash", "--bin", os.path.relpath(image, project)] + arguments, project)


def main(argv):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)   # our lines in order with the tools' output
    options = [a for a in argv if a.startswith("-")]
    words = [a for a in argv if not a.startswith("-")]
    start = "."
    if "-C" in argv:
        index = argv.index("-C")
        start = argv[index + 1] if index + 1 < len(argv) else "."
        words = [w for w in words if w != start]
    command = words[0] if words else "help"
    if command in ("help", "-h") or "--help" in options or "-h" in options:
        print(__doc__.strip())
        return 0
    project = find_project(start)
    if command == "doctor":
        results = checks(project, updates_too=True)
        if "--json" in options:
            print(json.dumps({"project": project, "hal": HAL, "checks": results}, indent=2))
        else:
            print_checks(results)
            if not signed_in() and find_203c() and sys.stdin.isatty() and not os.environ.get("CI"):
                try:
                    answer = input("Sign in to the 203 community now (opens the browser)? [Y/n] ").strip().lower()
                except EOFError:
                    answer = "n"
                if answer in ("", "y", "yes"):
                    through_203c(["login"], project)
        return 1 if any(check["status"] == "missing" for check in results) else 0
    if command in ("build", "web") and not project:
        need(project, checks(project))
        return 1
    if command == "build":
        return build(project, options)
    if command == "web":
        return web(project)
    if command == "flash":
        return flash(project, argv[argv.index("flash") + 1:])
    if command == "log":
        return through_203c(["log"] + argv[argv.index("log") + 1:], project)
    sys.stderr.write("cardenza: unknown command %r\n\n%s\n" % (command, __doc__.strip()))
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
