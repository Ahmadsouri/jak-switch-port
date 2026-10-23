# Jak 1 Switch Installer — desktop app

A small Windows application that turns a copy of the game the player already owns into the exact
folder tree the Switch port loads, and then helps move it onto the SD card. It is the graphical
version of `setup/prepare-sd.bat`: no command line, no devkitPro, no build tools.

It runs **only on the PC**. It never touches the console, and it ships no game data — it drives the
official OpenGOAL tools against the player's own disc image.

```
┌────────────────────────────────────────────────────────────────┐
│ 1. Your game      pick the .iso (drag & drop works)            │
│ 2. Prepare        download tools → read disc → ARM64 compile   │
│ 3. Copy to Switch finished folder + one-click copy to the card │
└────────────────────────────────────────────────────────────────┘
```

## What it does

1. **Finds the OpenGOAL tools.** Downloads `opengoal-windows-v0.3.8.zip` once into the work folder
   (about 43 MB) and reuses it afterwards.
2. **Copies the compiler project** (`goal_src`, `game`, `decompiler`, `custom_assets`) out of the
   installer's resources into the work folder, because the extractor writes into the project.
3. **Extracts the disc** (`-e -v`), 338 files.
4. **Reads the level data** (`-d`). This is the step that produces `out/jak1/fr3`; a compile alone
   only ever writes the test-zone `.fr3`.
5. **Compiles for ARM64** (`-c --instruction-set arm64`), 1039 `obj` + 322 `iso` + 26 `fr3`.
6. **Assembles `switch/jak1`** — `gk.nro`, `data/game`, `data/custom_assets` and the compiled
   `data/out/jak1` in one folder, plus a bilingual `HOW-TO-PLAY.txt`.
7. **Copies to a card** when a reader is plugged in, with byte progress and a file-count check.

The window is bilingual (Arabic/English, with the whole layout flipping to RTL) and remembers the
language, the last disc image, both folders and the reuse setting in
`%APPDATA%/Jak 1 Switch Installer/settings.json`.

Progress is measured, not faked: the app samples the output folders on disk while the extractor
runs (`iso_data/jak1` for extraction, `out/jak1/fr3` for the level pass, `out/jak1/obj` for the
compile), so the bar and ETA follow real files appearing.

Re-running is cheap: an already extracted disc or an already compiled `out/jak1` is detected and
skipped. **Cancel** stops the running extractor immediately; the work folder keeps whatever was
already produced, so the next run continues from there.

## Building it

```bash
cd app
npm install
npm run icon                                     # renders build/icon.png with Electron
JAK1_NRO=/c/jak-switch-build/gk.nro npm run resources   # stages resources/proj + resources/runtime
npm test                                         # 11 unit tests, no Electron needed
npm run dist                                     # dist/Jak1-Switch-Installer-<version>.exe
```

`npm run resources` copies about 214 MB (the compiler project plus `gk.nro` and the runtime
`game/` + `custom_assets/` folders) out of the checkout. Those copies are git-ignored; only the
script that creates them is versioned. `JAK1_NRO` may be any `gk.nro` you built with
`tools/switch/build-switch.sh` + `tools/switch/package-nro.sh`.

The packaged installer is unsigned, so Windows SmartScreen shows a warning on first launch
("More info" → "Run anyway"). Code signing would need a paid certificate.

## Layout

```
app/
├── main.js               window, IPC, settings, drive detection, copy with progress
├── preload.js            the only bridge the page gets (contextIsolation on)
├── src/pipeline.js       the install pipeline; no Electron imports, unit-tested directly
├── src/strings.js        every string, in English and Arabic
├── renderer/             index.html, styles.css, app.js (screens, i18n, events)
├── tools/                stage-resources.js, make-icon.js
└── test/                 node --test suite
```

`--smoke` starts the app, waits for the page to signal it rendered, prints `SMOKE OK` and exits.
It is what the release build is checked with; a renderer error makes it exit non-zero.

## Limits

- Windows x64 only.
- Needs a connection for the first run (the 43 MB tool download).
- About 6 GB free: ~4 GB in the work folder, ~1.9 GB in the output folder.
- Reading the SD card needs a reader Windows can see as a drive. The Switch in MTP mode is not a
  drive, so use the card in a reader (or the folder picker for a mounted path).
