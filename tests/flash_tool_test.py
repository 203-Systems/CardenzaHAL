"""tools/cardenza_flash.py without a board: reading partition tables and app images, finding the
Launcher, and the build tables that would overwrite it."""
import importlib.util
import os
import struct
import sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("cardenza_flash", os.path.join(here, "..", "tools", "cardenza_flash.py"))
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


def table(*entries):
    data = b"".join(struct.pack("<2sBBLL16sL", b"\xaa\x50", t, s, o, n, name.encode(), 0) for name, t, s, o, n in entries)
    return data + b"\xeb\xeb" + b"\xff" * 30 + b"\xff" * 64


LAUNCHER = table(("nvs", 1, 2, 0x9000, 0x5000), ("otadata", 1, 0, 0xE000, 0x2000), ("app0", 0, 0x20, 0x10000, 0x150000),
                 ("coredump", 1, 3, 0x160000, 0x10000), ("app_1", 0, 0x10, 0x170000, 0x200000))
DEFAULT = table(("nvs", 1, 2, 0x9000, 0x5000), ("otadata", 1, 0, 0xE000, 0x2000), ("launcher", 1, 0x40, 0x10000, 0x150000),
                ("coredump", 1, 3, 0x160000, 0x10000), ("ota_0", 0, 0x10, 0x170000, 0x400000))
FACTORY = table(("nvs", 1, 2, 0x9000, 0x6000), ("phy_init", 1, 1, 0xF000, 0x1000), ("factory", 0, 0x00, 0x10000, 0x400000))

entries = tool.parse_table(LAUNCHER)
assert [e["name"] for e in entries] == ["nvs", "otadata", "app0", "coredump", "app_1"]
assert tool.launcher_slot(entries)["size"] == 0x150000
assert tool.launcher_slot(tool.parse_table(DEFAULT)) is None
assert tool.launcher_slot(tool.parse_table(FACTORY)) is None
assert tool.parse_table(b"\xff" * 64) == []

# The default table and the Launcher's own are safe; a factory app at 0x10000 is not.
assert tool.table_problems(tool.parse_table(DEFAULT)) == []
assert tool.table_problems(tool.parse_table(LAUNCHER)) == []
problems = tool.table_problems(tool.parse_table(FACTORY))
assert any("factory" in p and "launcher" in p for p in problems)
assert any("nvs" in p for p in problems)  # 24 KB of NVS runs into the Launcher's otadata

# An app image: its descriptor names it; an erased slot is no image.
head = bytearray(0x100)  # an image: its descriptor's strings are NUL padded
head[0] = 0xE9
struct.pack_into("<L", head, 32, 0xABCD5432)
head[48:48 + 5] = b"0.2.0"
head[80:80 + 11] = b"launcher203"
assert tool.describe_image(bytes(head)) == "launcher203 0.2.0"
assert tool.describe_image(b"\xff" * 0x100) is None
# --app-into: the app image of a build, and an app partition that can take it.
FLASH_ARGS = "--flash_mode dio --flash_freq 80m --flash_size 16MB\n0x0 bootloader/bootloader.bin\n0x170000 MyFirmware.bin\n0x8000 partition_table/partition-table.bin\n0xe000 ota_data_initial.bin\n"
assert tool.app_image(FLASH_ARGS) == "MyFirmware.bin"
assert tool.app_image("--flash_mode dio\n0x0 bootloader/bootloader.bin\n") is None
launcher_entries = tool.parse_table(LAUNCHER)
slot, why = tool.app_slot(launcher_entries, "app_1", 0x100000)
assert slot is not None and slot["offset"] == 0x170000 and why is None
assert tool.app_slot(launcher_entries, "app0", 0x1000)[0] is None  # the Launcher itself
assert "does not fit" in tool.app_slot(launcher_entries, "app_1", 0x300000)[1]
assert "not an app" in tool.app_slot(launcher_entries, "nvs", 1)[1]
assert "no partition" in tool.app_slot(launcher_entries, "nothing", 1)[1]
print("flash tool ok")
