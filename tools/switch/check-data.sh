#!/usr/bin/env bash
# Check that a data tree has everything the Switch build needs, without touching the console.
#
# Usage:  bash tools/switch/check-data.sh <data-dir>      (default: ./data, then ./)
#
# The runtime loads compiled data from <data>/out/jak1; it never compiles GOAL on the console, so a
# missing out/jak1 is the single most common reason a correct gk.nro "closes immediately".
set -u
DATA="${1:-}"
if [ -z "$DATA" ]; then
  for candidate in ./data .; do
    [ -d "$candidate/out/jak1" ] && { DATA="$candidate"; break; }
  done
fi
DATA="${DATA:-./data}"

fail=0
ok()   { printf '  ok    %s\n' "$1"; }
bad()  { printf '  MISS  %s\n' "$1"; fail=1; }

echo "== checking $DATA"
[ -d "$DATA" ] || { echo "  no such directory"; exit 1; }

[ -f "$DATA/out/jak1/iso/KERNEL.CGO" ] && ok "out/jak1/iso/KERNEL.CGO" || bad "out/jak1/iso/KERNEL.CGO"
[ -d "$DATA/out/jak1/obj" ]  && ok "out/jak1/obj"  || bad "out/jak1/obj"
[ -d "$DATA/out/jak1/fr3" ]  && ok "out/jak1/fr3"  || bad "out/jak1/fr3"
[ -d "$DATA/out/jak1/iso" ]  && ok "out/jak1/iso"  || bad "out/jak1/iso"
[ -d "$DATA/game" ]          && ok "game/"         || bad "game/ (shaders, fonts)"
[ -f "$DATA/game/assets/sdl_controller_db.txt" ] \
  && ok "game/assets/sdl_controller_db.txt" || printf '  warn  game/assets/sdl_controller_db.txt missing (controller DB)\n'

for sub in obj iso fr3; do
  [ -d "$DATA/out/jak1/$sub" ] || continue
  n=$(find "$DATA/out/jak1/$sub" -type f | wc -l)
  printf '  info  %s/ files: %s\n' "$sub" "$n"
  case "$sub" in
    obj) [ "$n" -ge 900 ] || printf '  warn  far fewer than the ~1039 files a complete jak1 build produces\n' ;;
    iso) [ "$n" -ge 300 ] || printf '  warn  far fewer than the ~322 level files a complete build produces\n' ;;
    fr3) [ "$n" -ge 20 ]  || printf '  warn  level art is missing: the -d extraction step did not run before -c\n' ;;
  esac
done

# The data must be ARM64: a stray x86 build loads but the first GOAL call dies.
probe=$(ls "$DATA"/out/jak1/obj/*.o 2>/dev/null | head -1)
if [ -n "$probe" ] && command -v xxd >/dev/null; then
  first=$(xxd -l 8 -p "$probe")
  case "$first" in
    7f454c46*) printf '  info  probe %s = ELF object\n' "$(basename "$probe")" ;;
    *)         printf '  info  probe %s starts %s\n' "$(basename "$probe")" "$first" ;;
  esac
fi

if [ "$fail" -ne 0 ]; then
  echo "== incomplete: see GAME-DATA.md for the extraction and ARM64 build steps"
  exit 1
fi
echo "== data tree looks complete"
