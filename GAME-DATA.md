# Game data — bring your own

The desktop installer ([`app/`](app/)) performs every step on this page for you: pick the ISO, wait,
and it writes the finished `switch/jak1` folder. The commands below are the manual equivalent, for
building on Linux/macOS or for understanding what the app is doing.

This project ships **no game data**. Everything the console loads is built on a PC from a copy of
*Jak and Daxter: The Precursor Legacy* that you own, and it stays on your own SD card.

The release archive has a tool that does all of this:
`setup\prepare-sd.bat` (source: [`tools/install/`](../tools/install/)). Double-click it, point it at
your ISO, wait a few minutes, copy the resulting `switch` folder to the SD card root. The rest of
this page is what that script does, for people who want to run the steps by hand or on Linux.

## What you need

- The **USA** disc: `SCUS_97124`, *NTSC-U*, Rev 1. Other regions are rejected by the extractor.
- The official OpenGOAL tools, release **v0.3.8** (`opengoal-windows-v0.3.8.zip`,
  `extractor.exe` from it). It is the version this port's data was built and tested with, and it is
  the only piece the script downloads.
- ~6 GB of free disk space.

## Step 1 — extract the ISO (once)

```bash
extractor.exe --proj-path <proj> --extract-path <proj>/iso_data -e -v --disable-ansi \
  "Jak and Daxter - The Precursor Legacy (USA) (En,Fr,De,Es,It) (Rev 1).iso"
```

`<proj>` is a folder that contains `goal_src/` (the game sources from this repo), `decompiler/config/`
and `game/`. The release archive ships exactly that folder as `setup\proj`. Output is 338 files,
~1.4 GB, under `iso_data/jak1`.

## Step 2 — read the level data (must come before step 3)

```bash
extractor.exe --proj-path <proj> -g jak1 -d --disable-ansi \
  --decomp-config-override '{"decompile_code": false, "levels_extract": true, "allowed_objects": []}' \
  <proj>/iso_data/jak1
```

`decompile_code: false` skips the (unused) code decompile and keeps only the level data the compiler
needs. Apart from `decompiler_out/`, this step is what writes **`out/jak1/fr3/`** (the 25 levels'
art). Compiling on its own only ever produces `test-zone.fr3`, and the console then fails on the
first level load — so if you skip this step you get a data tree that looks complete and is not.

## Step 3 — compile for ARM64

```bash
mkdir -p out/jak1/obj out/jak1/iso out/jak1/fr3      # the compiler does not create them
extractor.exe --proj-path <proj> -g jak1 -c --instruction-set arm64 --disable-ansi <proj>/iso_data/jak1
```

`--instruction-set arm64` is mandatory; the default is x86 and x86 data kills the first GOAL call.
A complete build is **1039** object files, **322** level files and **26** `.fr3` files (25 from step
2 plus `test-zone`). `obj/` and `iso/` are reproducible; `fr3/` differs slightly between runs
(texture packing order), which is expected and harmless.

## Step 4 — copy to the SD card

```
sdmc:/switch/jak1/gk.nro
sdmc:/switch/jak1/data/game/          (shaders, fonts, sdl_controller_db.txt)
sdmc:/switch/jak1/data/custom_assets/
sdmc:/switch/jak1/data/out/jak1/      (obj/ iso/ fr3/ — ~1.8 GB)
```

Check a data tree before flashing it:

```bash
bash tools/switch/check-data.sh <data>/out/..
```

## Why this file exists

`.gitignore` ignores everything in `iso_data/`, `out/` and `switch/`, so extracted or compiled game
data can never be committed by accident. If you find game data in a pull request, do not merge it.
