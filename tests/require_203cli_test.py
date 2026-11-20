"""cmake/require_203cli.cmake, the check every firmware's build makes (idf.py build, tools/cardenza.py build, the web
target): without 203cli the configure stops and says how to install it; 203cli on PATH or the Agent Skill's copy run by
node passes; an old 203cli warns; one that does not run stops; CI skips the check. Where only 203c (the old name) is
installed, on PATH or as an older skill's script, the build passes with a warning to install 203cli. A 203cli (or the
last 203c, 3.5.1) is asked for its update notices, which become a warning (nothing when it prints none, fails or
hangs); an older 203c is not asked. The old file name (require_203c.cmake, cardenza_hal_require_203c) still works.
Runs the check in CMake's script mode with a PATH that holds only what each case puts there."""
import os
import shutil
import subprocess
import sys
import tempfile

here = os.path.dirname(os.path.abspath(__file__))
check = os.path.join(here, "..", "cmake", "require_203cli.cmake").replace("\\", "/")
old_check = os.path.join(here, "..", "cmake", "require_203c.cmake").replace("\\", "/")
cmake = shutil.which("cmake")
node = shutil.which("node")
if not cmake:
    print("require_203cli: cmake not found, skipped")
    sys.exit(0)
if os.name == "nt":
    print("require_203cli: the fake 203cli here is a shell script, skipped on Windows")
    sys.exit(0)

work = tempfile.mkdtemp(prefix="require-203cli-")
script = os.path.join(work, "run.cmake")
with open(script, "w", encoding="utf-8") as f:
    f.write('include("%s")\ncardenza_hal_require_203cli()\nmessage(STATUS "passed")\n' % check)
old_script = os.path.join(work, "run-old.cmake")
with open(old_script, "w", encoding="utf-8") as f:
    f.write('include("%s")\ncardenza_hal_require_203c()\nmessage(STATUS "passed")\n' % old_check)


def run(name, bin_files=None, env=None, skill=None, cmake_script=script):
    """Runs the check with a PATH of one folder holding bin_files {name: shell script}; skill {script name: text}."""
    case = os.path.join(work, name)
    bin_dir = os.path.join(case, "bin")
    os.makedirs(bin_dir)
    for file, body in (bin_files or {}).items():
        path = os.path.join(bin_dir, file)
        if body is None:
            os.symlink(node, path)
            continue
        with open(path, "w", encoding="utf-8") as f:
            f.write("#!/bin/sh\n" + body + "\n")
        os.chmod(path, 0o755)
    home = os.path.join(case, "home")
    os.makedirs(home)
    for file, text in (skill or {}).items():
        target = os.path.join(home, ".claude", "skills", "203-community", "scripts")
        os.makedirs(target, exist_ok=True)
        with open(os.path.join(target, file), "w", encoding="utf-8") as f:
            f.write(text)
    environment = {"PATH": bin_dir, "HOME": home}
    environment.update(env or {})
    result = subprocess.run([cmake, "-P", cmake_script], env=environment, capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


INSTALL = "npm install -g 203cli && 203cli skill install"
RENAMED = "203c is renamed 203cli"

code, out = run("missing")
assert code != 0 and "203cli is not installed" in out and INSTALL in out and "never idf.py flash" in out, out
assert "passed" not in out, out

code, out = run("on-path", {"203cli": 'echo "3.5.1"'})
assert code == 0 and "passed" in out and "older" not in out and RENAMED not in out, out

code, out = run("newer", {"203cli": 'echo "203cli 4.0.1"'})
assert code == 0 and "passed" in out and "older" not in out, out

code, out = run("old", {"203cli": 'echo "3.5.0"'})
assert code == 0 and "passed" in out and "203cli 3.5.0 is older" in out and INSTALL in out, out

# The old name only: the build goes on, with the warning to install 203cli. 203cli is preferred when both are there.
code, out = run("old-name", {"203c": 'echo "3.5.0"'})
assert code == 0 and "passed" in out and RENAMED in out and INSTALL in out and "is older" not in out, out
code, out = run("both", {"203c": 'echo "3.5.0"', "203cli": 'echo "3.5.1"'})
assert code == 0 and "passed" in out and RENAMED not in out, out

# 203cli's update notices (203cli updates --quiet --project <the project>): a warning with its lines.
NOTICE = "203cli 3.6.0 is out (you have 3.5.1): npm install -g 203cli && 203cli skill install"
BEHIND = "CardenzaHAL in this project (components/CardenzaHAL) is 3 commits behind its main: git submodule update --remote components/CardenzaHAL"
UPDATES = 'case "$1" in --version) echo 3.5.1;; updates) echo "$@" > "$HOME/asked"; %s;; esac'
code, out = run("notices", {"203cli": UPDATES % ('echo "%s"; echo "%s"' % (NOTICE, BEHIND))})
assert code == 0 and "passed" in out and "CMake Warning" in out and NOTICE in out and BEHIND in out, out
with open(os.path.join(work, "notices", "home", "asked"), encoding="utf-8") as f:
    asked = f.read().split()
assert asked[:3] == ["updates", "--quiet", "--project"] and len(asked) == 4, asked

# The last 203c (3.5.1, 203cli under the old name): asked too; its rename line is not said twice.
RENAME_LINE = "203c is renamed 203cli; 203c gets no more updates or support. Run: " + INSTALL
code, out = run("last-203c", {"203c": UPDATES % ('echo "%s"; echo "%s"' % (RENAME_LINE, BEHIND))})
assert code == 0 and "passed" in out and BEHIND in out and out.count(RENAMED) == 1, out

code, out = run("no-notices", {"203cli": UPDATES % "true"})
assert code == 0 and "passed" in out and "CMake Warning" not in out, out
assert os.path.exists(os.path.join(work, "no-notices", "home", "asked")), "asked"

code, out = run("updates-fails", {"203cli": UPDATES % 'echo "Unknown command: updates"; exit 2'})
assert code == 0 and "passed" in out and "CMake Warning" not in out and "Unknown command" not in out, out

code, out = run("updates-hangs", {"203cli": UPDATES % "exec /bin/sleep 30"})
assert code == 0 and "passed" in out and "CMake Warning" not in out, out

# An older 203c (before 3.5.1): not asked (it has no `updates`).
code, out = run("before-updates", {"203c": 'case "$1" in --version) echo 3.5.0;; *) echo asked > "$HOME/asked";; esac'})
assert code == 0 and "passed" in out and RENAMED in out and "from 203c updates" not in out, out
assert not os.path.exists(os.path.join(work, "before-updates", "home", "asked")), "a 203c without updates was asked"

code, out = run("broken", {"203cli": "echo 'node: not found' >&2; exit 127"})
assert code != 0 and "does not run" in out and INSTALL in out, out

code, out = run("ci", {"203cli": UPDATES % ('echo "%s"' % NOTICE)}, env={"CI": "true"})
assert code == 0 and "passed" in out and NOTICE not in out, out
assert not os.path.exists(os.path.join(work, "ci", "home", "asked")), "asked in CI"

code, out = run("github-actions", env={"GITHUB_ACTIONS": "true"})
assert code == 0 and "passed" in out, out

# The old file name, for a build that includes it by name.
code, out = run("old-file", {"203cli": 'echo "3.5.1"'}, cmake_script=old_script)
assert code == 0 and "passed" in out, out

if node:
    code, out = run("skill-copy", {"node": None}, skill={"203cli.mjs": 'console.log("3.5.1");\n', "203c.mjs": 'console.log("3.5.1");\n'})
    assert code == 0 and "passed" in out and RENAMED not in out, out
    code, out = run("old-skill-copy", {"node": None}, skill={"203c.mjs": 'console.log("3.5.0");\n'})
    assert code == 0 and "passed" in out and RENAMED in out, out
    code, out = run("skill-without-node", skill={"203cli.mjs": 'console.log("3.5.1");\n'})
    assert code != 0 and "203cli is not installed" in out, out

shutil.rmtree(work, ignore_errors=True)
print("require_203cli: missing, on PATH, newer, old, the old name, the update notices, broken, CI, the old file and the skill's copy passed")
