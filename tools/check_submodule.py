#!/usr/bin/env python3
"""Checks this HAL where a firmware uses it as a git submodule: a clean consumer copy, changed only
upstream (in a clone of CardenzaHAL, pushed to its main) and pulled in by moving the submodule pointer.

usage: python3 <HAL>/tools/check_submodule.py [--fetch]

It fails (exit 1, saying why) when the HAL has local changes, or is on a commit upstream main does not
have (a local commit, or a branch that is not merged). It also sets the HAL's push URL to no-push://, so
nothing is ever pushed from inside a firmware. --fetch fetches upstream before saying a commit is not
there. A HAL that is not a git checkout (copied into components/) is not checked. Run it before every
firmware build (from make, or CMake's execute_process).
"""

import os
import subprocess
import sys

NO_PUSH = "no-push://CardenzaHAL-is-a-read-only-dependency"


def git(hal, *arguments):
    return subprocess.run(["git", "-C", hal] + list(arguments), capture_output=True, text=True)


def main():
    hal = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if not os.path.exists(os.path.join(hal, ".git")):
        print("check_submodule: %s is not a git checkout; not checked" % hal)
        return 0
    git(hal, "config", "remote.origin.pushurl", NO_PUSH)
    changes = git(hal, "status", "--porcelain").stdout.strip()
    if changes:
        sys.stderr.write("check_submodule: the HAL (%s) has local changes, but it is a clean copy. Change the HAL in\n"
                         "its own clone, push it to CardenzaHAL's main, then move the submodule pointer. Restore this one:\n"
                         "  git -C %s checkout -- . && git -C %s clean -fd\n%s\n" % (hal, hal, hal, changes))
        return 1
    on_main = git(hal, "merge-base", "--is-ancestor", "HEAD", "origin/main").returncode == 0
    if not on_main and "--fetch" in sys.argv[1:]:
        git(hal, "fetch", "-q", "origin")
        on_main = git(hal, "merge-base", "--is-ancestor", "HEAD", "origin/main").returncode == 0
    if not on_main:
        head = git(hal, "log", "--oneline", "-1").stdout.strip()
        sys.stderr.write("check_submodule: the HAL (%s) is on %s, which is not on upstream main.\n"
                         "Push the change to CardenzaHAL's main first, then move the submodule pointer to it.\n" % (hal, head))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
