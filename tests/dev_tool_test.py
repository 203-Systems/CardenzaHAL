"""tools/cardenza.py (the preferred build tool) without ESP-IDF or a board: the project and its BIN found, ESP-IDF 5.3
found where it is installed (activated or not), 203c found on PATH or in the skill's folder, the sign-in, the checks
a build stops at, and doctor's update notices (203c updates --json, from a 203c that has it)."""
import importlib.util
import io
import json
import os
import sys
import tempfile
from contextlib import redirect_stderr, redirect_stdout

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("cardenza", os.path.join(here, "..", "tools", "cardenza.py"))
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def idf(root, version):
    major, minor, patch = version.split(".")
    write(os.path.join(root, "tools", "cmake", "version.cmake"),
          "set(IDF_VERSION_MAJOR %s)\nset(IDF_VERSION_MINOR %s)\nset(IDF_VERSION_PATCH %s)\n" % (major, minor, patch))
    write(os.path.join(root, "export.sh"), "")


with tempfile.TemporaryDirectory() as folder:
    # The project: the folder with 203.json, from below it; its project() and its BIN.
    project = os.path.join(folder, "metronome")
    write(os.path.join(project, "CMakeLists.txt"), "cmake_minimum_required(VERSION 3.16)\nproject(pocket_metronome)\n")
    write(os.path.join(project, "203.json"), "{}")
    os.makedirs(os.path.join(project, "main"))
    assert tool.find_project(os.path.join(project, "main")) == project
    assert tool.project_name(project) == "pocket_metronome"
    assert tool.built_bin(project) == os.path.join(project, "build", "pocket_metronome.bin")
    write(os.path.join(project, "build", "project_description.json"), json.dumps({"app_bin": "other.bin"}))
    assert tool.built_bin(project) == os.path.join(project, "build", "other.bin"), "the build's own name wins"
    assert tool.find_project(folder) is None

    # ESP-IDF: 5.3.5 preferred among the installed ones; one activated is taken as it is (doctor says when it is not 5.3).
    home = os.path.join(folder, "home")
    idf(os.path.join(home, "esp", "esp-idf-v5.3.5"), "5.3.5")
    idf(os.path.join(home, "esp", "esp-idf-v5.4.1"), "5.4.1")
    idf(os.path.join(home, "esp", "esp-idf-v5.3.2"), "5.3.2")
    path, version, activated = tool.find_idf(env={"PATH": ""}, home=home)
    assert (version, activated) == ("5.3.5", False) and path.endswith("esp-idf-v5.3.5"), (path, version)
    assert tool.idf_version(os.path.join(home, "esp", "esp-idf-v5.4.1")) == "5.4.1"
    path, version, activated = tool.find_idf(env={"PATH": "", "IDF_PATH": os.path.join(home, "esp", "esp-idf-v5.4.1")}, home=home)
    assert version == "5.4.1", "the IDF_PATH set is the one used"
    assert tool.find_idf(env={"PATH": ""}, home=os.path.join(folder, "nobody")) == (None, None, False)

    # 203c: on PATH, else the skill's own script with node.
    bin_dir = os.path.join(folder, "bin")
    assert tool.find_203c(env={"PATH": bin_dir}, home=home) is None
    write(os.path.join(bin_dir, "203c"), "#!/bin/sh\necho 3.5.0\n")
    os.chmod(os.path.join(bin_dir, "203c"), 0o755)
    found = tool.find_203c(env={"PATH": bin_dir}, home=home)
    assert found and found[0].endswith("203c"), found
    assert tool.version_tuple("3.5.0") >= tool.CLI_WANTED > tool.version_tuple("3.4.0")

    # The sign-in: COMMUNITY_TOKEN, else the token file 203c keeps.
    assert not tool.signed_in(env={}, home=home)
    assert tool.signed_in(env={"COMMUNITY_TOKEN": "x"}, home=home)
    write(os.path.join(folder, "config", "203c", "token"), "cct_x\n")
    assert tool.signed_in(env={"XDG_CONFIG_HOME": os.path.join(folder, "config")}, home=home)

    # doctor asks a 203c that has `updates` (3.5.1 or later) for its notices: each one a warning, the one about the HAL's
    # local changes left out (check_submodule.py's "hal" line says it); all current is one line; an older 203c, or a
    # build's checks, do not ask.
    LOCAL = "the HAL (x) has local changes, but it is a clean copy"
    notices = ["203c 3.6.0 is out (you have 3.5.1): npm install -g 203c && 203c skill install",
               "CardenzaHAL in this project (components/CardenzaHAL) is 3 commits behind its main: git submodule update --remote components/CardenzaHAL",
               "CardenzaHAL in this project (components/CardenzaHAL) is not a clean copy of upstream main: " + LOCAL]
    answers = {
        "behind": {"cli": {"current": "3.5.1", "latest": "3.6.0", "outdated": True},
                   "hal": {"path": "components/CardenzaHAL", "status": "ahead", "behind": 3, "local": LOCAL}, "notices": notices},
        "current": {"cli": {"current": "3.5.1", "latest": "3.5.1", "outdated": False},
                    "hal": {"path": "components/CardenzaHAL", "status": "identical", "behind": 0, "local": None}, "notices": []},
        "offline": {"cli": {"current": "3.5.1", "latest": None, "outdated": False}, "hal": None, "notices": []},
    }
    asked = os.path.join(folder, "asked")

    def doctor_updates(version, answer):
        updates_bin = os.path.join(folder, "bin-" + version + "-" + answer)
        write(os.path.join(updates_bin, "answer.json"), json.dumps(answers[answer]))
        write(os.path.join(updates_bin, "203c"), '#!/bin/sh\ncase "$1" in --version) echo %s;; updates) echo "$@" > "%s"; cat "%s";; esac\n'
              % (version, asked, os.path.join(updates_bin, "answer.json")))
        os.chmod(os.path.join(updates_bin, "203c"), 0o755)
        os.environ["PATH"] = updates_bin + os.pathsep + os.path.dirname(sys.executable)
        if os.path.exists(asked):
            os.remove(asked)
        return [c for c in tool.checks(project, updates_too=True) if c["name"] == "updates"]

    saved_path = os.environ.get("PATH", "")
    got = doctor_updates("3.5.1", "behind")
    assert [c["status"] for c in got] == ["warn", "warn"] and "3.6.0 is out" in got[0]["detail"] and "3 commits behind" in got[1]["detail"], got
    with open(asked, encoding="utf-8") as f:
        assert f.read().split() == ["updates", "--json", "--project", project]
    got = doctor_updates("3.5.1", "current")
    assert [(c["status"], c["detail"]) for c in got] == [("ok", "203c 3.5.1 is the newest; CardenzaHAL (components/CardenzaHAL) is at its main")], got
    assert [c["status"] for c in doctor_updates("3.5.1", "offline")] == ["skip"]
    assert doctor_updates("3.5.0", "behind") == [] and not os.path.exists(asked), "a 203c without updates is not asked"
    doctor_updates("3.5.1", "behind")
    os.remove(asked)
    assert not [c for c in tool.checks(project) if c["name"] == "updates"] and not os.path.exists(asked), "a build's checks do not ask"
    os.environ["PATH"] = saved_path

    # A build stops at what is missing and says how to install it; CI may build without 203c.
    os.environ["PATH"] = os.path.join(folder, "empty")
    results = tool.checks(project)
    by = {c["name"]: c for c in results}
    assert by["203c"]["status"] == "missing" and "npm install -g 203c" in by["203c"]["fix"], by["203c"]
    assert by["version.txt"]["status"] == "warn"
    assert all("fix" not in c for c in results if c["status"] == "ok")
    assert {c["name"]: c for c in tool.checks(project, need_cli=False)}["203c"]["status"] == "warn"
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        assert tool.main(["build", "-C", folder]) == 1, "no project here"
        assert tool.main(["help"]) == 0
        assert tool.main(["frobnicate"]) == 2
    assert "doctor" in out.getvalue() and "unknown command" in err.getvalue()

print("tools/cardenza.py: project, BIN, ESP-IDF, 203c, sign-in, checks and doctor's update notices passed")
sys.exit(0)
