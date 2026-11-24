# Firmware template

A starting point for a Cardenza firmware on CardenzaHAL, laid out the way
[`docs/FIRMWARE-PROJECT.md`](../../docs/FIRMWARE-PROJECT.md) (the rules every firmware on the HAL
follows) asks: the HAL as a git submodule, the board's defaults, a partition table that keeps the
203 Launcher, `version.txt`, `CHANGELOG.md` and `203.json` for publishing with `203cli`. The app shows
the keys on the screen and lights a held key; ok (enter) inverts the screen; HOME is the system's (a short press opens
the system menu, HOME + space takes a screenshot into `/sdcard/screenshot/`); on the SD card it keeps its files in
its own folder, `/sdcard/Apps/<project name>/` (`Settings/app.cfg`, `Logs/log.txt`: [`docs/SD-CARD.md`](../../docs/SD-CARD.md)). The same source builds for the
board and for the browser.

## Start a firmware from it

```sh
npm install -g 203cli && 203cli skill install      # 203cli is required: a firmware does not build without it
203cli new "My Firmware" --summary "What it does"  # 203-CLI: https://github.com/203-Systems/203-CLI
```

makes `my-firmware/` from it: a git repository, CardenzaHAL as the submodule `components/CardenzaHAL`, the project
named (`project()` in `CMakeLists.txt`, `publish.bins` in `203.json`, `elf:` in the workflow), `203.json` filled in.
Then add a `LICENSE` file for your code (and its SPDX id in `203.json` `license`), replace `main/main.cpp`, keep
`version.txt` (`major.minor.patch`) and a `## <version>` entry in `CHANGELOG.md` for every release. By hand instead:
[`docs/FIRMWARE-PROJECT.md`](../../docs/FIRMWARE-PROJECT.md#start-a-project).

## Build and try

```sh
python3 components/CardenzaHAL/tools/cardenza.py doctor   # what this computer still needs
python3 components/CardenzaHAL/tools/cardenza.py build    # build/my_firmware.bin (ESP-IDF 5.3, exported if needed)
python3 components/CardenzaHAL/tools/cardenza.py web      # build-web/site
python3 components/CardenzaHAL/tools/cardenza_web.py serve build-web/site   # http://localhost:8000/
203cli flash --bin build/my_firmware.bin                   # on a Cardenza, through the 203 Launcher (never idf.py flash)
203cli log --until "my_firmware 0.1.0 ready" --seconds 30  # it started
```

Inside the CardenzaHAL repository it builds in place: `python3 tools/cardenza.py build -C examples/template`
(or `idf.py -C examples/template build`), `python3 tools/cardenza_web.py build examples/template`.

## Publish

The page first: people who are not developers decide there whether to install it.

- `docs/description.md` is its description (`publish.description_file`): fill in every `<…>` of the template, short
  and for those people: what it is, 3–6 features, what it needs, how to start (aim for 800 characters; the guide:
  `203cli help description`, <https://cardenza.203.io/community/guide/description>). This README is for developers.
- 3–5 different screenshots of it in `203.json`'s `publish.images`, e.g. `["docs/screens/1.png", "docs/screens/2.png",
  "docs/screens/3.png"]`: `203cli board screen --out docs/screens/1.png` takes one from a Cardenza on USB running it
  (CardenzaHAL::Init starts USB, so it works with this template as it is). `203cli publish`
  needs them.

The first version by hand (it makes the firmware and writes its code into `203.json`: commit that):

```sh
203cli login
203cli publish --dry-run
203cli publish
```

Then every release comes from CI, the standard way for every firmware: `.github/workflows/firmware.yml`
publishes a push to `main` as a dev version, a tag `vX.Y.Z` (equal to `version.txt`, with its
`CHANGELOG.md` entry) as stable and `vX.Y.Z-beta.N` as beta, with the HAL's `community-publish` action and
no secret (trusted publishing). Link the repository once on the firmware's page (自动发布 / Trusted
publishing: this repository, workflow `firmware.yml`) and turn beta and dev on there.

Everything else (setup, the rules, what the community checks, versions, forks): [`docs/FIRMWARE-PROJECT.md`](../../docs/FIRMWARE-PROJECT.md).
