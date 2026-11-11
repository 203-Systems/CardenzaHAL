"""tools/cardenza_usb.py and tools/check_submodule.py without a board."""
import importlib.util
import os
import subprocess
import sys
import tempfile

here = os.path.dirname(os.path.abspath(__file__))
tools = os.path.join(here, "..", "tools")
spec = importlib.util.spec_from_file_location("cardenza_usb", os.path.join(tools, "cardenza_usb.py"))
usb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(usb)

# SysEx answers: whole messages come out, a message cut in two waits for its end, others are skipped.
prefix = bytes([0xF0, 0x24, 0x7F])
found, rest = usb.split_sysex(b"\xf0\x24\x7fG!1\xf7\xf0\x24\x43xx\xf7\xf0\x24\x7fGS", prefix)
assert found == [b"G!1"] and rest == b"\xf0\x24\x7fGS", (found, rest)
found, rest = usb.split_sysex(rest + b"1\xf7", prefix)
assert found == [b"GS1"] and rest == b"", (found, rest)
found, rest = usb.split_sysex(b"\x00\xf0\x24", prefix)  # a prefix cut between two reads is kept
assert found == [] and rest == b"\xf0\x24"
assert usb.app_exchange(b"x" * 64)[0] is None  # longer than a report
assert set(usb.COMMANDS) >= {"identify", "reboot", "launcher", "download", "console"}

# check_submodule: a clean checkout on main passes; local changes and a commit off main fail.
with tempfile.TemporaryDirectory() as folder:
    upstream, hal = os.path.join(folder, "upstream"), os.path.join(folder, "hal")
    def run(*command, cwd=None):
        return subprocess.run(list(command), cwd=cwd, capture_output=True, text=True)
    env = ["-c", "user.name=t", "-c", "user.email=t@t", "-c", "init.defaultBranch=main"]
    run("git", *env, "init", "-q", upstream)
    os.makedirs(os.path.join(upstream, "tools"))
    with open(os.path.join(tools, "check_submodule.py")) as source, open(os.path.join(upstream, "tools", "check_submodule.py"), "w") as copy:
        copy.write(source.read())
    run("git", *env, "add", "-A", cwd=upstream)
    run("git", *env, "commit", "-q", "-m", "init", cwd=upstream)
    run("git", "clone", "-q", upstream, hal)
    check = [sys.executable, os.path.join(hal, "tools", "check_submodule.py")]
    assert run(*check).returncode == 0
    assert run("git", "-C", hal, "config", "remote.origin.pushurl").stdout.strip().startswith("no-push://")
    with open(os.path.join(hal, "local.txt"), "w") as file:
        file.write("x")
    assert run(*check).returncode == 1
    os.remove(os.path.join(hal, "local.txt"))
    run("git", *env, "commit", "-q", "--allow-empty", "-m", "local", cwd=hal)
    result = run(*check)
    assert result.returncode == 1 and "not on upstream main" in result.stderr
print("host tools ok")
