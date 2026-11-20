"""cmake/require_203c.cmake, the check every firmware's build makes (idf.py build, tools/cardenza.py build, the web
target): without 203c the configure stops and says how to install it; 203c on PATH or the Agent Skill's copy run by
node passes; an old 203c warns; one that does not run stops; CI skips the check. Runs the check in CMake's script
mode with a PATH that holds only what each case puts there."""
import os
import shutil
import subprocess
import sys
import tempfile

here = os.path.dirname(os.path.abspath(__file__))
check = os.path.join(here, "..", "cmake", "require_203c.cmake").replace("\\", "/")
cmake = shutil.which("cmake")
node = shutil.which("node")
if not cmake:
    print("require_203c: cmake not found, skipped")
    sys.exit(0)
if os.name == "nt":
    print("require_203c: the fake 203c here is a shell script, skipped on Windows")
    sys.exit(0)

work = tempfile.mkdtemp(prefix="require-203c-")
script = os.path.join(work, "run.cmake")
with open(script, "w", encoding="utf-8") as f:
    f.write('include("%s")\ncardenza_hal_require_203c()\nmessage(STATUS "passed")\n' % check)


def run(name, bin_files=None, env=None, skill=None):
    """Runs the check with a PATH of one folder holding bin_files {name: shell script}."""
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
    if skill:
        target = os.path.join(home, ".claude", "skills", "203-community", "scripts")
        os.makedirs(target)
        with open(os.path.join(target, "203c.mjs"), "w", encoding="utf-8") as f:
            f.write(skill)
    environment = {"PATH": bin_dir, "HOME": home}
    environment.update(env or {})
    result = subprocess.run([cmake, "-P", script], env=environment, capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


INSTALL = "npm install -g 203c && 203c skill install"

code, out = run("missing")
assert code != 0 and "203c is not installed" in out and INSTALL in out and "never idf.py flash" in out, out
assert "passed" not in out, out

code, out = run("on-path", {"203c": 'echo "3.5.0"'})
assert code == 0 and "passed" in out and "older" not in out, out

code, out = run("newer", {"203c": 'echo "203c 4.0.1"'})
assert code == 0 and "passed" in out and "older" not in out, out

code, out = run("old", {"203c": 'echo "3.4.2"'})
assert code == 0 and "passed" in out and "203c 3.4.2 is older" in out and INSTALL in out, out

code, out = run("broken", {"203c": "echo 'node: not found' >&2; exit 127"})
assert code != 0 and "does not run" in out and INSTALL in out, out

code, out = run("ci", env={"CI": "true"})
assert code == 0 and "passed" in out, out

code, out = run("github-actions", env={"GITHUB_ACTIONS": "true"})
assert code == 0 and "passed" in out, out

if node:
    code, out = run("skill-copy", {"node": None}, skill='console.log("3.5.0");\n')
    assert code == 0 and "passed" in out, out
    code, out = run("skill-without-node", skill='console.log("3.5.0");\n')
    assert code != 0 and "203c is not installed" in out, out

shutil.rmtree(work, ignore_errors=True)
print("require_203c: missing, on PATH, newer, old, broken, CI and the skill's copy passed")
