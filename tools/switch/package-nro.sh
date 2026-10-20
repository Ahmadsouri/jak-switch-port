#!/usr/bin/env bash
# Package the built game ELF (game/gk) into gk.nro.
#
# Usage:  bash tools/switch/package-nro.sh [output-dir]
#
# No CMake rule produces the .nro: elf2nro has to run after the link, against the ELF and a
# NACP descriptor. The NACP is created once if it does not exist yet.
set -eu
source "$(cd "$(dirname "$0")" && pwd)/switch-env.sh"

BUILD_DIR="$SWITCH_BUILD_DIR"
OUT_DIR="${1:-$BUILD_DIR}"
ELF="$BUILD_DIR/game/gk"
NACP="$BUILD_DIR/control.nacp"

[ -f "$ELF" ] || { echo "no ELF at $ELF -- run tools/switch/build-switch.sh gk first"; exit 1; }

if [ ! -f "$NACP" ]; then
  echo "== creating NACP"
  "$DEVKITPRO/tools/bin/nacptool.exe" --create "OpenGOAL Jak 1" "switch-port" "1.0.0" "$NACP"
fi

echo "== packaging $ELF -> $OUT_DIR/gk.nro"
"$DEVKITPRO/tools/bin/elf2nro.exe" "$ELF" "$OUT_DIR/gk.nro" \
  --icon="$DEVKITPRO/libnx/default_icon.jpg" --nacp="$NACP"

printf '== %s  %s bytes  sha256 %s\n' \
  "$OUT_DIR/gk.nro" "$(stat -c%s "$OUT_DIR/gk.nro")" \
  "$(sha256sum "$OUT_DIR/gk.nro" | cut -d' ' -f1)"
