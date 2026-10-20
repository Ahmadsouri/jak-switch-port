# OpenGOAL — Jak 1 on Nintendo Switch

An ARM64 homebrew port of [OpenGOAL](https://github.com/open-goal/jak-project) running **Jak and Daxter:
The Precursor Legacy** natively on Horizon OS (libnx + SDL2 + GLES2). No Android/Linux layer, no x86
emulation of game code: the GOAL runtime is compiled for `aarch64-none-elf` and the game's compiled
data (`out/jak1`) is built with `--instruction-set arm64`.

Branch base: `06890f31` (kept verbatim in this repo's initial commit) — "Switch port: run Jak 1 as ARM64 homebrew under Horizon/GLES".

## Status

| Item | State |
|---|---|
| Boot on hardware | ✅ reaches gameplay |
| Render | ✅ 1280×720 (`[SWITCH-RES] render target 1280x720`) |
| Audio | ✅ |
| Controller input | ✅ positional mapping (SOUTH = jump) |
| Save / load | ✅ after the fixes below |
| Start-menu stutter | ✅ fixed (per-frame card probe → 1 Hz cache) |
| Latest **verified** build | **build29** — `sha256 e6fd4ecf14309169…` |
| Latest **built** build | build30 — flashed and read back byte-identical, **not yet verified in play** |

build30 differs from build29 by one change: `pc_read_bank_header()` stages through a heap buffer
instead of a stack array (same class of failure as the 64 KB stack buffer fixed in build28 — the FS
service rejects transfer buffers that live inside the emulated EE memory region).

## Requirements

- **devkitPro** with: `devkita64-gcc` 16.x, `libnx` 4.x, `switch-sdl2`, `switch-mesa`,
  `switch-libdrm_nouveau`, `switch-tools`, `switch-pkg-config`.
- **CMake** 3.20+ and **Ninja** (portable copies work — see `tools/switch/switch-env.sh`).
- A legally owned copy of the game for asset extraction. **No game data is in this repository.**

## Build

```bash
# 1. environment (override DEVKITPRO / SWITCH_BUILD_DIR / SWITCH_CMAKE / SWITCH_NINJA as needed)
source tools/switch/switch-env.sh

# 2. compile: JIT smoke test + the game ELF
bash tools/switch/build-switch.sh            # or: build-switch.sh gk
#   -> $SWITCH_BUILD_DIR/game/gk   (default build dir: C:/jak-switch-build)

# 3. package (no CMake rule does this; elf2nro runs post-link)
bash tools/switch/package-nro.sh             # -> $SWITCH_BUILD_DIR/gk.nro
```

Two link-time gates run on every build and fail it rather than shipping a broken binary:

- `cmake/check_readonly_relocs.cmake` — refuses relocations that target read-only segments.
- `cmake/check_exec_sections.cmake` — refuses code sections that land in a non-executable segment.

### Windows note

Windows resolves junctions when it sets a process's working directory, so the **build directory must
be a short real path** (`C:/jak-switch-build`), otherwise object paths exceed `MAX_PATH` and `ar`
fails on draco. Reaching the *source* through a short junction (`C:\jaksrc`) is fine. Set
`MSYS2_ARG_CONV_EXCL='*'` when scripting Windows tools from Git Bash (`/E` becomes `E:/`).

## Game data

The runtime does not compile GOAL on the console — it loads data built on the host:

```bash
# decompiler outputs the compiler needs (tpage-dir, entities, raw_obj)
extractor.exe --proj-path <data> -g jak1 -d --disable-ansi \
  --decomp-config-override '{"decompile_code": false, "levels_extract": true, "allowed_objects": []}' \
  <data>/iso_data/jak1

# ARM64 build of the game's data (~1.8 GB, 1318 targets)
extractor.exe --proj-path <data> -g jak1 -c --instruction-set arm64 --disable-ansi <data>/iso_data/jak1
```

`--instruction-set arm64` is mandatory; the tool defaults to x86.

Step-by-step, plus the SD layout and a pre-flash check
(`bash tools/switch/check-data.sh <data-dir>`): [GAME-DATA.md](GAME-DATA.md).

## SD card layout

```
sdmc:/
├── gk_boot_log.txt              # boot trace (created on every run)
├── gk_stdout.txt                # stdout, including the fmt/stdio crash path
└── switch/jak1/
    ├── gk.nro
    └── data/
        ├── game/                # shaders (92), fonts, sdl_controller_db.txt
        ├── custom_assets/
        └── out/jak1/            # compiled game data -- REQUIRED
            ├── obj/   iso/   fr3/
```

Launch `gk.nro` from hbmenu; hold **R** on any installed title to get the full memory pool (the EE
memory reservation needs it).

## Diagnostics

- `sdmc:/gk_boot_log.txt` — stage markers (`[main]`, `[iop_runner]`, `[ee_runner]`, `[MC]` lines).
- `sdmc:/gk_stdout.txt` — stdout; the port redirects it to a file because `fmt::print` on the console's
  broken stdout aborted the process (`game/switch/stdout_sink.h`).
- `sdmc:/atmosphere/crash_reports/` — Atmosphère reports. A hang (no new report) and a crash (report)
  are different bugs; the port's history had both.

## How the hard problems were solved

| Symptom | Cause | Fix |
|---|---|---|
| Immediate Data Abort before `main` | libnx mis-decodes packed RELR relocations; writes into `.rodata` | `-Wl,-z,nopack-relative-relocs` (`game/CMakeLists.txt`) |
| Instruction Abort at `co_swap_function` | libco named its sections `.text#…`, so `ld` put them in a read-only segment | section renamed to `.text.co_…` (`third-party/libco/aarch64.c`) + exec-section gate |
| Save slot reads failed with `0xD401` | 64 KB **stack** staging buffer; the FS service refuses buffers inside the emulated EE region | heap `std::vector` staging with retries and real `errno` in the log (`common/util/FileUtil.cpp`) |
| `pc-settings.gc` never written | `sdmc:/…` paths re-prefixed because they are not `fs::is_absolute()` | device-prefix check in `get_file_path()` |
| "Error while saving" | `last_file` always `-1`; out-of-bounds guest file index; exceptions escaping guest calls | memory-card state machine repaired (`game/kernel/common/kmemcard.cpp`) |
| Start menu hitched every frame | card probe (8 opens + 2 bank reads per slot) ran on every `mc-get-slot-info` | `McSlotCache`, at most one probe per second, invalidated after every save/load |
| Boot log grew by thousands of duplicate lines | repeated identical `file_util` failures written every frame | `switch_log_file()` coalesces with `… [repeated] xN` |

## Known limitations

- **build30 is unverified on hardware** — it fixed the header-read staging buffer after finding 77/77
  probe reads failing in the build29 card log, but no post-flash log exists yet.
- A failed write can still reach `fmt::system_error` from the logging/stdout path (uncaught → abort
  with no crash report). This is the best remaining suspect for silent deaths.
- Intermediate builds (25–27) exist only as artifacts, not as source snapshots.
- Only **Jak 1** is wired up; Jak 2/3 data and save layouts are untouched.

## Legal

This repository contains porting code only. It ships no game assets, no ISO data and no compiled
game data. You need your own legally obtained copy of *Jak and Daxter: The Precursor Legacy* to
extract assets. OpenGOAL is licensed under ISC — see [LICENSE](LICENSE).
