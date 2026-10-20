#!/usr/bin/env bash
# Build the OpenGOAL Jak 1 Switch port into an ELF (see package-nro.sh for the .nro).
#
# Usage:  bash tools/switch/build-switch.sh [target ...]     (default: switch_jit_test then gk)
#
# Requires devkitPro (devkitA64, libnx, switch-sdl2, switch-mesa, switch-tools), a portable
# CMake/Ninja, and a SHORT REAL build directory -- see tools/switch/switch-env.sh.
set -u
source "$(cd "$(dirname "$0")" && pwd)/switch-env.sh"

SRC_DIR="$_REPO_ROOT"
BUILD_DIR="$SWITCH_BUILD_DIR"

# Windows MAX_PATH: if the checkout lives deep, build through a short junction instead of the
# real tree. `cygpath -w` converts the bash path for CMake/Ninja (both Windows binaries).
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) SRC_ARG="$(cygpath -w "$SRC_DIR")" ;;
  *)                     SRC_ARG="$SRC_DIR" ;;
esac

if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  echo "== configuring ($SRC_ARG -> $BUILD_DIR)"
  "$SWITCH_CMAKE" -S "$SRC_ARG" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$SRC_ARG/cmake/toolchains/Switch.cmake" \
    -DDEVKITPRO="$DEVKITPRO" \
    -DCMAKE_MAKE_PROGRAM="$SWITCH_NINJA" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSTATICALLY_LINK=ON \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    || { echo "configure failed"; exit 1; }
fi

TARGETS=("$@")
if [ ${#TARGETS[@]} -eq 0 ]; then TARGETS=(switch_jit_test gk); fi

cd "$BUILD_DIR" || exit 1
"$SWITCH_NINJA" -j "${JOBS:-10}" "${TARGETS[@]}"
rc=$?
echo "== ninja exit: $rc"
exit $rc
