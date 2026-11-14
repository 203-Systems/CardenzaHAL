"""tools/cardenza_web.py, the web target's build tool, without a board: sdkconfig, what a failed build
means, finding the HAL, the page and sim.json, the toolchain checks. With Emscripten installed it also
builds examples/psram_heap for the web (EndToEnd)."""
import hashlib
import importlib.util
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

here = os.path.dirname(os.path.abspath(__file__))
hal = os.path.dirname(here)
spec = importlib.util.spec_from_file_location("cardenza_web", os.path.join(hal, "tools", "cardenza_web.py"))
web = importlib.util.module_from_spec(spec)
spec.loader.exec_module(web)


def write(path, text=""):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as out:
        out.write(text)


class Sdkconfig(unittest.TestCase):
    def test_parse(self):
        values = web.parse_sdkconfig('CONFIG_A=y\nCONFIG_B=n\n# CONFIG_C is not set\nCONFIG_D=0x10\nCONFIG_E="text"\n'
                                     "  CONFIG_F = 12  \n# a comment\nNOT_CONFIG=1\n")
        self.assertEqual(values, {"CONFIG_A": "y", "CONFIG_B": None, "CONFIG_C": None, "CONFIG_D": "0x10",
                                  "CONFIG_E": '"text"', "CONFIG_F": "12"})

    def test_layers_and_choices(self):
        board = web.parse_sdkconfig("CONFIG_FREERTOS_HZ=1000\nCONFIG_SPIRAM=y\n")
        firmware = web.parse_sdkconfig("CONFIG_SPIRAM_USE_MEMMAP=y\nCONFIG_LOG_DEFAULT_LEVEL_WARN=y\n"
                                       "CONFIG_ESP_MAIN_TASK_AFFINITY_NO_AFFINITY=y\n")
        values = web.merge_sdkconfig([board, firmware])
        self.assertEqual(values["CONFIG_FREERTOS_HZ"], "1000")
        self.assertEqual(values["CONFIG_SPIRAM_USE_MEMMAP"], "y")
        self.assertNotEqual(values.get("CONFIG_SPIRAM_USE_MALLOC"), "y")
        self.assertIsNone(values["CONFIG_LOG_DEFAULT_LEVEL_INFO"])
        self.assertEqual(values["CONFIG_LOG_DEFAULT_LEVEL"], "2")
        self.assertEqual(values["CONFIG_LOG_MAXIMUM_LEVEL"], "2")
        self.assertEqual(values["CONFIG_ESP_MAIN_TASK_AFFINITY"], "0x7FFFFFFF")
        self.assertIsNone(values["CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0"])
        # ESP-IDF's defaults where nobody said otherwise.
        self.assertEqual(web.merge_sdkconfig([])["CONFIG_FREERTOS_HZ"], "100")
        self.assertEqual(web.merge_sdkconfig([])["CONFIG_IDF_TARGET"], '"esp32s3"')
        # PSRAM without a mode is in malloc, Kconfig's default.
        self.assertEqual(web.merge_sdkconfig([board])["CONFIG_SPIRAM_USE_MALLOC"], "y")

    def test_full_sdkconfig_wins(self):
        with tempfile.TemporaryDirectory() as folder:
            full, defaults = os.path.join(folder, "sdkconfig"), os.path.join(folder, "sdkconfig.defaults")
            write(defaults, "CONFIG_FREERTOS_HZ=1000\n")
            self.assertEqual(web.make_sdkconfig(full, [defaults])["CONFIG_FREERTOS_HZ"], "1000")
            write(full, "CONFIG_FREERTOS_HZ=500\n")
            self.assertEqual(web.make_sdkconfig(full, [defaults])["CONFIG_FREERTOS_HZ"], "500")

    def test_render(self):
        values = {"CONFIG_A": "y", "CONFIG_B": None, "CONFIG_N": "42", "CONFIG_S": '"esp32s3"'}
        header = web.render_sdkconfig_h(values)
        self.assertIn("#define CONFIG_A 1\n", header)
        self.assertIn("#define CONFIG_N 42\n", header)
        self.assertIn('#define CONFIG_S "esp32s3"\n', header)
        self.assertNotIn("CONFIG_B", header)
        cmake = web.render_sdkconfig_cmake(values)
        self.assertIn('set(CONFIG_A "y")', cmake)
        self.assertIn('set(CONFIG_B "")', cmake)
        self.assertIn('set(CONFIG_S "esp32s3")', cmake)


class Classify(unittest.TestCase):
    def test_unsupported(self):
        log = "\n".join([
            "wasm-ld: error: esp-idf/main/lib__idf_main.a(link.cpp.o): undefined symbol: esp_bt_controller_init",
            "error: undefined symbol: esp_wifi_start (referenced by root reference (e.g. compiled C/C++ code))",
            "main.cpp:3:52: error: 'gpio_sleep_sel_en' is unavailable: not provided by the CardenzaHAL web target",
            "lcd.cpp:2:5: error: 'esp_lcd_new_panel_st7789' is unavailable: set up by CardenzaHAL: use the handle the HAL gives",
            "wifi.c:1:10: fatal error: 'esp_wifi.h' file not found",
            "x.c:1:10: fatal error: 'soc/rtc_cntl_reg.h' file not found",
            "implicit.c:2:24: error: call to undeclared function 'esp_deep_sleep_start'; ISO C99 and later do not support",
            "a.cpp:9:3: error: use of undeclared identifier 'xRingbufferCreate'",
            "CMake Error at comp/CMakeLists.txt:1 (message): cardenza-web: unsupported-source: /p/main/asm.S (assembly for the chip)",
            "wasm-ld: error: x.o: undefined symbol: esp_bt_controller_init",
            "em++: error: '/emsdk/upstream/bin/wasm-ld -o fw.wasm ...' failed (returned 1)",
        ])
        unsupported, errors = web.classify_log(log)
        self.assertEqual(unsupported, [
            ("unsupported", "esp_bt_controller_init"), ("unsupported", "esp_wifi_start"), ("unsupported", "gpio_sleep_sel_en"),
            ("unsupported", "esp_lcd_new_panel_st7789"), ("unsupported-header", "esp_wifi.h"),
            ("unsupported-header", "soc/rtc_cntl_reg.h"), ("unsupported", "esp_deep_sleep_start"),
            ("unsupported", "xRingbufferCreate"), ("unsupported-source", "/p/main/asm.S")])
        self.assertEqual(errors, [])

    def test_own_errors(self):
        log = ("main.cpp:4:10: fatal error: 'my_lib.h' file not found\n"
               "main.cpp:7:3: error: use of undeclared identifier 'DrawScreen'\n"
               "main.cpp:9:1: error: expected ';' after expression\n"
               "warning: unused variable\n")
        unsupported, errors = web.classify_log(log)
        self.assertEqual(unsupported, [])
        self.assertEqual(len(errors), 3)

    def test_cpp_names(self):
        unsupported, _ = web.classify_log("wasm-ld: error: a.o: undefined symbol: Foo::Bar(int, char const*)\n")
        self.assertEqual(unsupported, [("unsupported", "Foo::Bar(int, char const*)")])

    def test_headers(self):
        for header in ("esp_wifi.h", "driver/uart.h", "esp_private/panic_internal.h", "xtensa_context.h", "tusb.h", "nvs_sec.h",
                       "sys/lock.h", "sys/reent.h", "xtensa/config/core-isa.h", "xt_utils.h", "esp_debug_helpers.h", "esp_cpu_utils.h",
                       "esp_heap_trace.h", "esp_core_dump.h", "esp_ota_ops.h", "esp_partition.h", "esp_flash.h", "spi_flash_mmap.h",
                       "esp_sleep.h", "esp_pm.h", "esp_bt_main.h", "esp_netif_types.h", "lwip/sockets.h", "mbedtls/sha256.h",
                       "tinyusb.h", "hal/gpio_ll.h", "soc/rtc_cntl_reg.h", "rom/cache.h", "esp32s3/rom/ets_sys.h"):
            self.assertTrue(web.idf_header(header), header)
        for header in ("my_lib.h", "lvgl.h", "Devices/Device.h", "sys/stat.h", "sys/time.h"):
            self.assertFalse(web.idf_header(header), header)


class Project(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.folder)

    def make_hal(self, path, with_web):
        write(os.path.join(path, "cardenzaHAL.h"))
        write(os.path.join(path, "CMakeLists.txt"))
        if with_web:
            write(os.path.join(path, "web", "idf", "tools", "cmake", "project.cmake"))

    def test_idf_project(self):
        project = os.path.join(self.folder, "fw")
        write(os.path.join(project, "CMakeLists.txt"), "include($ENV{IDF_PATH}/tools/cmake/project.cmake)\nproject(fw)\n")
        self.assertTrue(web.is_idf_project(project))
        write(os.path.join(self.folder, "plain", "CMakeLists.txt"), "project(plain)\n")
        self.assertFalse(web.is_idf_project(os.path.join(self.folder, "plain")))
        self.assertFalse(web.is_idf_project(os.path.join(self.folder, "none")))
        self.assertEqual(web.main(["build", os.path.join(self.folder, "plain")]), web.EXIT_USAGE)

    def test_find_hal(self):
        project = os.path.join(self.folder, "fw")
        self.make_hal(os.path.join(project, "build", "copy"), True)  # build folders are not looked into
        self.assertIsNone(web.find_project_hal(project))
        self.make_hal(os.path.join(project, "Devices", "Cardenza", "CardenzaHAL"), False)
        self.assertEqual(web.find_project_hal(project), os.path.join(project, "Devices", "Cardenza", "CardenzaHAL"))
        self.make_hal(os.path.join(project, "components", "CardenzaHAL"), True)
        self.assertEqual(web.find_project_hal(project), os.path.join(project, "components", "CardenzaHAL"))
        # The HAL's own examples: the HAL is above the project.
        example = os.path.join(self.folder, "hal", "examples", "one")
        os.makedirs(example)
        self.make_hal(os.path.join(self.folder, "hal"), True)
        self.assertEqual(web.find_project_hal(example), os.path.join(self.folder, "hal"))

    def test_choose_hal(self):
        own = os.path.join(self.folder, "own")
        self.make_hal(own, True)
        project = os.path.join(self.folder, "fw")
        self.make_hal(os.path.join(project, "components", "CardenzaHAL"), False)
        chosen, warning = web.choose_hal(project, own=own)
        self.assertEqual(chosen, own)
        self.assertIn("has no web target", warning)
        self.make_hal(os.path.join(project, "components", "CardenzaHAL"), True)
        self.assertEqual(web.choose_hal(project, own=own), (os.path.join(project, "components", "CardenzaHAL"), None))
        self.assertEqual(web.choose_hal(project, explicit=own, own=own), (own, None))
        with self.assertRaises(ValueError):
            web.choose_hal(project, explicit=self.folder, own=own)

    def test_assemble(self):
        hal_dir = os.path.join(self.folder, "hal")
        page = os.path.join(hal_dir, "web", "page")
        write(os.path.join(page, "index.html"), "<title>@CARDENZA_TITLE@ @CARDENZA_VERSION@</title><script src=\"@CARDENZA_MODULE@\"></script>")
        write(os.path.join(page, "cardenza-sim.js"), "// page")
        write(os.path.join(page, "keymap.json"), json.dumps({"keys": [["ESC", "Esc"], ["PLAY", "Space"]]}))
        write(os.path.join(page, "README.md"), "not shipped")
        site = os.path.join(self.folder, "site")
        write(os.path.join(site, "fw.js"), "module")
        write(os.path.join(site, "fw.wasm"), "wasm")
        write(os.path.join(site, "stale.txt"), "not ours")
        manifest = web.assemble({"name": "fw<1>", "version": "1.2.3", "hal_dir": hal_dir, "site_dir": site, "module": "fw.js",
                                 "emscripten": "6.0.13"})
        with open(os.path.join(site, "index.html")) as source:
            self.assertEqual(source.read(), '<title>fw&lt;1&gt; 1.2.3</title><script src="fw.js"></script>')
        with open(os.path.join(site, "sim.json")) as source:
            self.assertEqual(json.load(source), manifest)
        self.assertEqual(manifest["format"], 1)
        self.assertEqual(manifest["target"], "cardenza")
        self.assertEqual(manifest["entry"], "index.html")
        self.assertEqual(manifest["firmware"], {"name": "fw<1>", "version": "1.2.3"})
        self.assertEqual(manifest["headers"], {})
        self.assertEqual(manifest["keys"], [["ESC", "Esc"], ["PLAY", "Space"]])
        self.assertEqual(manifest["toolchain"], {"emscripten": "6.0.13"})
        self.assertEqual(set(manifest["hal"]), {"version", "commit", "dirty"})
        paths = [entry["path"] for entry in manifest["files"]]
        self.assertEqual(paths, ["index.html", "cardenza-sim.js", "fw.js", "fw.wasm"])
        wasm = next(entry for entry in manifest["files"] if entry["path"] == "fw.wasm")
        self.assertEqual(wasm, {"path": "fw.wasm", "size": 4, "sha256": hashlib.sha256(b"wasm").hexdigest()})

    def test_placeholder_page(self):
        site = os.path.join(self.folder, "site")
        write(os.path.join(site, "fw.js"), "module")
        manifest = web.assemble({"name": "fw", "version": "1", "hal_dir": os.path.join(self.folder, "nohal"), "site_dir": site})
        with open(os.path.join(site, "index.html")) as source:
            text = source.read()
        self.assertIn('<script src="fw.js"></script>', text)
        self.assertIn("cardenza_sim: 'ready'", text)
        self.assertEqual(manifest["keys"], [])


class Toolchain(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.mkdtemp()
        self.bin = os.path.join(self.folder, "bin")
        os.makedirs(self.bin)

    def tearDown(self):
        shutil.rmtree(self.folder)

    def tool(self, path, text="#!/bin/sh\n"):
        write(path, text)
        os.chmod(path, os.stat(path).st_mode | stat.S_IEXEC)

    def environ(self, **extra):
        return dict({"PATH": self.bin, "HOME": os.path.join(self.folder, "home")}, **extra)

    @unittest.skipIf(sys.platform == "win32", "shell scripts as tools")
    def test_missing_and_found(self):
        with self.assertRaises(web.ToolchainMissing) as missing:
            web.find_toolchain(self.environ())
        self.assertIn("emsdk install latest", str(missing.exception))
        emsdk = os.path.join(self.folder, "emsdk")
        self.tool(os.path.join(emsdk, "upstream", "emscripten", "emcmake"))
        with self.assertRaises(web.ToolchainMissing) as missing:
            web.find_toolchain(self.environ(EMSDK=emsdk))
        self.assertIn("CMake 3.20", str(missing.exception))
        self.tool(os.path.join(self.bin, "cmake"), "#!/bin/sh\necho 'cmake version 3.16.3'\n")
        with self.assertRaises(web.ToolchainMissing) as missing:
            web.find_toolchain(self.environ(EMSDK=emsdk))
        self.assertIn("too old", str(missing.exception))
        self.tool(os.path.join(self.bin, "cmake"), "#!/bin/sh\necho 'cmake version 3.30.2'\n")
        self.tool(os.path.join(self.bin, "ninja"))
        found = web.find_toolchain(self.environ(EMSDK=emsdk))
        self.assertEqual(found["emcmake"], os.path.join(emsdk, "upstream", "emscripten", "emcmake"))
        self.assertEqual(found["generator"], ["-G", "Ninja"])
        self.assertEqual(found["env"]["EMSDK"], emsdk)
        # ~/emsdk is where emsdk's own instructions put it.
        home_emsdk = os.path.join(self.folder, "home", "emsdk")
        self.tool(os.path.join(home_emsdk, "upstream", "emscripten", "emcmake"))
        self.assertEqual(web.find_toolchain(self.environ())["env"]["EMSDK"], home_emsdk)


def emscripten_available():
    try:
        web.find_toolchain()
        return True
    except web.ToolchainMissing:
        return False


@unittest.skipUnless(emscripten_available(), "Emscripten (and CMake) not installed")
class EndToEnd(unittest.TestCase):
    """examples/psram_heap, unchanged, built for the web and run in node."""

    def test_psram_heap(self):
        with tempfile.TemporaryDirectory() as folder:
            site = os.path.join(folder, "site")
            result = subprocess.run([sys.executable, os.path.join(hal, "tools", "cardenza_web.py"), "build",
                                     os.path.join(hal, "examples", "psram_heap"), "--out", site, "--build-dir",
                                     os.path.join(folder, "cmake")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            with open(os.path.join(site, "sim.json")) as source:
                manifest = json.load(source)
            self.assertEqual(manifest["firmware"]["name"], "psram_heap")
            for entry in manifest["files"]:
                self.assertTrue(os.path.isfile(os.path.join(site, entry["path"])), entry)
            node = shutil.which("node")
            if node is None:
                return
            script = ("const M = await require(process.argv[2])({cardenzaConfig: {card: 0}});\n"
                      "const end = Date.now() + 1500;\n"
                      "while (Date.now() < end) { M._cardenza_web_run(8); await new Promise(r => setTimeout(r, 5)); }\n"
                      "process.exit(0);\n")
            runner = os.path.join(folder, "run.cjs")
            write(runner, "(async () => {\n" + script + "})();\n")
            run = subprocess.run([node, runner, os.path.join(site, "psram_heap.js")], capture_output=True, text=True, timeout=60)
            self.assertIn("64 KB in PSRAM: yes", run.stdout, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
