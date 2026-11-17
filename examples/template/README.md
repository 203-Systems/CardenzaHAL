# Firmware template

A starting point for a Cardenza firmware on CardenzaHAL, laid out the way
[`docs/FIRMWARE-PROJECT.md`](../../docs/FIRMWARE-PROJECT.md) (the rules every firmware on the HAL
follows) asks: the HAL as a git submodule, the board's defaults, a partition table that keeps the
203 Launcher, `version.txt`, `CHANGELOG.md` and `203.json` for publishing with `203c`. The app shows
the keys on the screen and lights a held key; HOME inverts the screen. The same source builds for the
board and for the browser.

## Start a firmware from it

```sh
git clone --depth 1 https://github.com/203-Systems/CardenzaHAL.git /tmp/CardenzaHAL
cp -r /tmp/CardenzaHAL/examples/template my-firmware
cd my-firmware
git init
git submodule add https://github.com/203-Systems/CardenzaHAL.git components/CardenzaHAL
```

Then make it yours:

1. `CMakeLists.txt`: `project(my_firmware)` becomes your project's name (letters, digits, `_`), and
   `publish.bins` in `203.json` follows it (`build/<name>.bin`).
2. `203.json`: `name` (shown in the community and the Launcher), `summary`, `tags`; `source` once the
   repository is online; `license` once you chose one (`203c init` fills in what it can find).
3. Add a `LICENSE` file for your code (MIT, Apache-2.0, ...) and say it in `203.json` `license`.
4. `version.txt` (`major.minor.patch`) and a `## <version>` entry in `CHANGELOG.md` for every release.
5. Replace `main/main.cpp`.

## Build

```sh
idf.py set-target esp32s3            # once, in an ESP-IDF 5.3.5 shell
idf.py build                         # build/my_firmware.bin
python3 components/CardenzaHAL/tools/cardenza_web.py build .              # build-web/site
python3 components/CardenzaHAL/tools/cardenza_web.py serve build-web/site # http://localhost:8000/
```

Inside the CardenzaHAL repository it builds in place: `idf.py -C examples/template build`,
`python3 tools/cardenza_web.py build examples/template`.

## Publish

The first version by hand (it makes the firmware and writes its code into `203.json`: commit that):

```sh
203c login
203c publish --dry-run
203c publish
```

Then every release comes from CI, the standard way for every firmware: `.github/workflows/firmware.yml`
publishes a push to `main` as a dev version, a tag `vX.Y.Z` (equal to `version.txt`, with its
`CHANGELOG.md` entry) as stable and `vX.Y.Z-beta.N` as beta, with the HAL's `community-publish` action and
no secret (trusted publishing). Link the repository once on the firmware's page (自动发布 / Trusted
publishing: this repository, workflow `firmware.yml`) and turn beta and dev on there.

Everything else (setup, the rules, what the community checks, versions, forks): [`docs/FIRMWARE-PROJECT.md`](../../docs/FIRMWARE-PROJECT.md).
