# Environment for building the OpenGOAL Jak 1 Switch port with devkitPro.
# Source this from a bash shell (Git Bash on Windows). Every path is overridable.

# Prefer a usable devkitPro: the one already exported, else the Windows default, else /opt.
for _dkp in "${DEVKITPRO:-}" "C:/devkitPro" "/opt/devkitpro"; do
  if [ -n "$_dkp" ] && [ -d "$_dkp/libnx" ]; then DEVKITPRO="$_dkp"; break; fi
done
export DEVKITPRO="${DEVKITPRO:-C:/devkitPro}"
export DEVKITA64="$DEVKITPRO/devkitA64"
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"

# Portable CMake/Ninja (no system install required). Drop your own copies under
# tools/portable/{cmake,ninja}, or set SWITCH_CMAKE / SWITCH_NINJA -- otherwise the
# cmake and ninja already on PATH are used.
_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [ -z "${SWITCH_CMAKE:-}" ]; then
  if [ -x "$_REPO_ROOT/tools/portable/cmake/bin/cmake.exe" ]; then
    SWITCH_CMAKE="$_REPO_ROOT/tools/portable/cmake/bin/cmake.exe"
  else
    SWITCH_CMAKE="$(command -v cmake)"
  fi
fi
if [ -z "${SWITCH_NINJA:-}" ]; then
  if [ -x "$_REPO_ROOT/tools/portable/ninja/ninja.exe" ]; then
    SWITCH_NINJA="$_REPO_ROOT/tools/portable/ninja/ninja.exe"
  else
    SWITCH_NINJA="$(command -v ninja)"
  fi
fi
export SWITCH_CMAKE SWITCH_NINJA

# Build directory. On Windows this must be a SHORT REAL path, not the source path:
# Windows resolves junctions/links when it sets a process working directory, so a build
# directory reached through a link produces object paths over MAX_PATH (260) and `ar`
# fails on draco. Keep it short and real, e.g. C:/jak-switch-build.
export SWITCH_BUILD_DIR="${SWITCH_BUILD_DIR:-C:/jak-switch-build}"
