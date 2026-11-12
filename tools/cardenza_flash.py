#!/usr/bin/env python3
"""Flash a firmware build to a Cardenza without erasing the 203 Launcher.

usage: python3 <HAL>/tools/cardenza_flash.py -p PORT [BUILD_DIR] [--check-only] [--replace-launcher]
       python3 <HAL>/tools/cardenza_flash.py -p PORT [BUILD_DIR] --app-into LABEL

The device's partition table belongs to the Launcher. A firmware installed through the Launcher goes
into a slot the Launcher makes; flashing a build directly (idf.py flash) writes its own bootloader and
partition table instead, and on a board that has the Launcher that erases the Launcher and its list of
installed software. So before writing anything this reads the board (in download mode):

- The board has the Launcher (an app partition of subtype test at 0x10000 with an image in it): a full
  flash is refused. Either install the firmware through the Launcher (copy BUILD_DIR/<name>.bin to the
  SD card and install it from the Launcher's file browser, or use the Launcher's serial installer), or,
  while developing, write only the app image into one of the Launcher's app partitions with
  --app-into LABEL (the Launcher's developer workflow: back in the Launcher, start or register it).
  --replace-launcher flashes everything anyway, erasing the Launcher on purpose.
- Otherwise it writes the build as idf.py flash would (BUILD_DIR/flash_args).

It also checks the build's own partition table: a table that puts anything other than the Launcher's
regions below 0x170000 would overwrite the Launcher on a board that gets one later
(board/partitions.csv does not). --check-only reads and reports, writing nothing.

A board running a firmware with USB on has no serial port: put it in download mode first
(tools/cardenza_usb.py command download, or hold BOOT while plugging it in).
Needs esptool (ESP-IDF's Python environment has it).
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

TABLE_OFFSET = 0x8000
TABLE_SIZE = 0xC00
ENTRY = struct.Struct("<2sBBLL16sL")
ENTRY_MAGIC = b"\xaa\x50"
TYPE_APP = 0x00
SUBTYPE_TEST = 0x20
APP_FIRST = 0x170000
# The Launcher's fixed regions (203-launcher boards/cardenza/partitions.csv): name, offset, size.
LAUNCHER_REGIONS = [
    ("nvs", 0x9000, 0x5000),
    ("otadata", 0xE000, 0x2000),
    ("launcher", 0x10000, 0x150000),
    ("coredump", 0x160000, 0x10000),
]


def parse_table(data):
    """Entries of an ESP partition table binary: dicts of name, type, subtype, offset, size."""
    entries = []
    for start in range(0, len(data) - ENTRY.size + 1, ENTRY.size):
        magic, ptype, subtype, offset, size, label, _flags = ENTRY.unpack_from(data, start)
        if magic != ENTRY_MAGIC:
            break  # 0xFFFF ends the table, 0xEBEB is its MD5
        name = label.split(b"\0", 1)[0].decode("ascii", "replace")
        entries.append({"name": name, "type": ptype, "subtype": subtype, "offset": offset, "size": size})
    return entries


def launcher_slot(entries):
    """The Launcher's own partition: an app of subtype test at 0x10000, or None."""
    for entry in entries:
        if entry["type"] == TYPE_APP and entry["subtype"] == SUBTYPE_TEST and entry["offset"] == 0x10000:
            return entry
    return None


def describe_image(head):
    """'name version' from the start of an app image (its esp_app_desc_t), or None when there is none."""
    if len(head) < 112 or head[0] != 0xE9:
        return None
    if struct.unpack_from("<L", head, 32)[0] != 0xABCD5432:
        return "an app"
    text = lambda raw: raw.split(b"\0", 1)[0].split(b"\xff", 1)[0].decode("ascii", "replace")
    version, project = text(head[48:80]), text(head[80:112])
    return (project + " " + version).strip()


def app_image(flash_args_text):
    """The app image of a build: the file flash_args writes that is not the bootloader, the partition
    table or the initial otadata."""
    for line in flash_args_text.splitlines()[1:]:
        parts = line.split()
        if len(parts) == 2 and not parts[1].startswith(("bootloader/", "partition_table/")) and parts[1] != "ota_data_initial.bin":
            return parts[1]
    return None


def app_slot(entries, label, image_size):
    """The board's app partition named label, or (None, why it cannot take the image)."""
    for entry in entries:
        if entry["name"] != label:
            continue
        if entry["type"] != TYPE_APP:
            return None, "%s is not an app partition" % label
        if entry["subtype"] == SUBTYPE_TEST or entry["offset"] < APP_FIRST:
            return None, "%s is the Launcher's own partition" % label
        if image_size > entry["size"]:
            return None, "the image (%d bytes) does not fit %s (%d bytes)" % (image_size, label, entry["size"])
        return entry, None
    return None, "the board has no partition %s (labels: %s)" % (label, ", ".join(e["name"] for e in entries if e["type"] == TYPE_APP))


def table_problems(entries):
    """What in a build's table would overwrite the Launcher on a board that has it."""
    problems = []
    for entry in entries:
        start, end = entry["offset"], entry["offset"] + entry["size"]
        for name, offset, size in LAUNCHER_REGIONS:
            if start < offset + size and offset < end and (start, end) != (offset, offset + size):
                problems.append("%s (0x%x..0x%x) overlaps the Launcher's %s (0x%x..0x%x)" % (
                    entry["name"], start, end, name, offset, offset + size))
        if entry["type"] == TYPE_APP and start < APP_FIRST and not (entry["subtype"] == SUBTYPE_TEST and start == 0x10000):
            problems.append("%s: a firmware below 0x%x is where the Launcher lives" % (entry["name"], APP_FIRST))
    return problems


def esptool(port, *arguments, cwd=None):
    command = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "-p", port] + list(arguments)
    return subprocess.run(command, cwd=cwd).returncode


def read_flash(port, offset, size):
    with tempfile.TemporaryDirectory() as folder:
        path = os.path.join(folder, "read.bin")
        if esptool(port, "--before", "default_reset", "--after", "no_reset", "read_flash", hex(offset), hex(size), path) != 0:
            return None
        with open(path, "rb") as file:
            return file.read()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-p", "--port", required=True)
    parser.add_argument("build", nargs="?", default="build")
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--replace-launcher", action="store_true")
    parser.add_argument("--app-into", metavar="LABEL", help="write only the app image into this app partition of the board")
    args = parser.parse_args()

    flash_args = os.path.join(args.build, "flash_args")
    if not os.path.isfile(flash_args):
        sys.exit("cardenza_flash: %s missing: build the firmware first (idf.py build)" % flash_args)
    built_table = os.path.join(args.build, "partition_table", "partition-table.bin")
    if os.path.isfile(built_table):
        with open(built_table, "rb") as file:
            for problem in table_problems(parse_table(file.read())):
                print("cardenza_flash: warning: the build's partition table: " + problem)

    table = read_flash(args.port, TABLE_OFFSET, TABLE_SIZE)
    if table is None:
        sys.exit("cardenza_flash: could not read the board (is it in download mode on %s?)" % args.port)
    entries = parse_table(table)
    if args.app_into:
        with open(flash_args) as file:
            image = app_image(file.read())
        if image is None:
            sys.exit("cardenza_flash: %s names no app image" % flash_args)
        target, why = app_slot(entries, args.app_into, os.path.getsize(os.path.join(args.build, image)))
        if target is None:
            sys.exit("cardenza_flash: not flashing: " + why)
        print("cardenza_flash: writing %s alone into %s at 0x%x; the bootloader and the partition table stay" % (
            image, target["name"], target["offset"]))
        if args.check_only:
            return 0
        result = esptool(args.port, "-b", "460800", "--before", "default_reset", "--after", "hard_reset", "write_flash",
                         hex(target["offset"]), image, cwd=args.build)
        if result == 0:
            print("cardenza_flash: done. In the Launcher, start it from the list, or register it (203-launcher tools/register_app.py).")
        return result
    slot = launcher_slot(entries)
    image = None
    if slot is not None:
        head = read_flash(args.port, slot["offset"], 0x100)
        image = describe_image(head) if head is not None else "unreadable"
    if image is not None:
        print("cardenza_flash: this board has the 203 Launcher (%s)." % image)
        if not args.replace_launcher:
            sys.exit("cardenza_flash: not flashing: a full flash would erase the Launcher and its list of installed software.\n"
                     "  Install the firmware through the Launcher: copy %s to the SD card and install it from the\n"
                     "  Launcher's file browser, or use the Launcher's serial installer. While developing,\n"
                     "  --app-into LABEL writes only the app into one of the Launcher's app partitions.\n"
                     "  --replace-launcher flashes anyway and erases the Launcher." % os.path.join(args.build, "<name>.bin"))
        print("cardenza_flash: --replace-launcher: the Launcher will be erased.")
    else:
        print("cardenza_flash: no Launcher on this board.")
    if args.check_only:
        return 0
    return esptool(args.port, "-b", "460800", "--before", "default_reset", "--after", "hard_reset", "write_flash", "@flash_args",
                   cwd=args.build)


if __name__ == "__main__":
    sys.exit(main())
