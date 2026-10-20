# Game data — bring your own

`iso_data/` in this repository is **empty on purpose**. It is where *you* put the files extracted
from a copy of *Jak and Daxter: The Precursor Legacy* that you own. No game data is distributed with
this repository, and none may be committed to it — everything under `iso_data/` is git-ignored.

```
iso_data/
└── jak1/
    └── (extractor output: CGO/ DGO/ MUS/ STR/ VAG/ ... SCUS_971.24 buildinfo.json)
```

## Step 1 — extract (on the host, once)

Use the official OpenGOAL `extractor.exe` against your own ISO:

```bash
./extractor.exe --proj-path <repo> --extract-path <repo>/iso_data -e -v --disable-ansi \
  "/path/to/Jak and Daxter - The Precursor Legacy (USA) (En,Fr,De,Es,It) (Rev 1).iso"
```

The extractor verifies the disc: region `NTSC-U`, serial `SCUS-97124`, `ntsc_v2` config. Output for
this title is 338 files (~1.4 GB).

## Step 2 — compile the data for the console

The Switch build needs ARM64 GOAL data, not the x86 data a desktop build produces:

```bash
extractor.exe --proj-path <data> -g jak1 -d --disable-ansi \
  --decomp-config-override '{"decompile_code": false, "levels_extract": true, "allowed_objects": []}' \
  <data>/iso_data/jak1

extractor.exe --proj-path <data> -g jak1 -c --instruction-set arm64 --disable-ansi <data>/iso_data/jak1
```

`--instruction-set arm64` is mandatory.

## Step 3 — copy to the SD card

```
sdmc:/switch/jak1/data/out/jak1/     (obj/ iso/ fr3/ — ~1.8 GB)
sdmc:/switch/jak1/data/game/         (shaders, fonts, sdl_controller_db.txt)
sdmc:/switch/jak1/data/custom_assets/
sdmc:/switch/jak1/gk.nro
```

Verify a data tree before flashing with `bash tools/switch/check-data.sh <data-dir>`.

## Why this file exists

`.gitignore` ignores everything in `iso_data/` except the `.gitignore` placeholders, so extracted
data can never be committed by accident. If you find game data in a pull request, do not merge it.
