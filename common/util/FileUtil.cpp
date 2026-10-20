/*!
 * @file FileUtil.cpp
 * Utility functions for reading and writing files.
 */

#include "FileUtil.h"

#include <algorithm>
#include <cstdio> /* defines FILENAME_MAX */
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#include "BinaryWriter.h"

#include "common/common_types.h"
#include "common/util/BinaryReader.h"
#include "common/util/Timer.h"
#include "common/util/string_util.h"
#include "common/util/unicode_util.h"

// This disables the use of PCLMULQDQ which is probably ok, but let's just be safe and disable it
// because nobody will care if png compression is 10% slower.
#define FPNG_NO_SSE 1
#include "fmt/format.h"
#include "third-party/fpng/fpng.cpp"
#include "third-party/fpng/fpng.h"
#include "third-party/lzokay/lzokay.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <cstring>
#include <unistd.h>
#endif
#ifdef __SWITCH__
#include <cerrno>
#include <cstdio>
#include <fcntl.h>

// See runtime.cpp / CodeTester.h: libnx's types.h typedefs u128 as __uint128_t while this
// project has a struct u128, so the names are swapped for the duration of the include.
#define u128 nx_u128
#define Semaphore nx_Semaphore
#include <switch.h>
#undef u128
#undef Semaphore

#include "game/switch/boot_log.h"
// log.h is pulled in here as well as below, because the failure lines at the bottom of this block
// have to reach the game's own log file. That is the channel with a proven record on this card
// (newlib writes it from every thread, all run long); the SD-card boot log has arrived as a
// 0-byte file on every run so far, so a failure reported only there is a failure nobody sees.
#include "common/log/log.h"
// The SD-card log is the only channel that survives a crash on this port (see boot_log.h). Every
// file that fails to read is worth naming there: the graphics loader runs on its own thread and
// turns an exception out of this file into a process-wide abort().
static void switch_log_file(const std::string& msg) {
  if (switch_process_dying().load(std::memory_order_relaxed)) {
    // See switch_process_dying(): once a fatal path has latched it, libnx is already tearing its
    // devices down and any write here becomes a Data Abort in _write_r instead of a log line.
    return;
  }
  // Rate-limit bursts of the *same* line. build29's live problem drew this in: the memory-card
  // probe (once per frame while a menu was open -- see kmemcard.cpp's cache note) re-opened four
  // save bank files it already knew were absent, every frame, and each absent open produced two
  // identical lines here -- 1,568 lines of "cannot open .../bank N.bin (open rc=0x202)" in one
  // run. The flood was not just noise: every line is two card chances (fs service, then stdio)
  // plus a flushed file write, executed from the guest call inside the frame budget, which is
  // measured in the game log as 130-177 ms kernel dispatch per frame while a menu was open.
  //
  // The window is deliberately small (1 s) and the cap generous (3): a genuinely new filesystem
  // problem still logs on its first three appearances of every second, so a flapping shader
  // open or a fresh missing file never goes silent -- only the march of identical repeats is
  // compressed. The "first of the burst carries the count" shape keeps the trace honest about
  // how often the condition is really happening.
  static std::string last_msg;
  static int repeats = 0;
  static Timer since_last;
  if (msg == last_msg && since_last.getSeconds() < 1.0) {
    if (repeats < 3) {
      // Log the offender this once with the accumulating count so the reader sees the burst.
      const std::string counted =
          msg + " [repeated]" + (repeats ? " x" + std::to_string(repeats + 1) : "");
      switch_boot_log_event(("[file_util] " + counted + "\n").c_str());
      lg::error("[file_util] {}", counted);
      repeats++;
      since_last.start();
    }
    // Past 3 in this window: swallow silently. The next unique message (or the window expiring)
    // restores reporting.
    return;
  }
  last_msg = msg;
  repeats = 0;
  since_last.start();
  switch_boot_log_event(("[file_util] " + msg + "\n").c_str());
  lg::error("[file_util] {}", msg);
}
#endif
#include "common/log/log.h"
#include "common/util/Assert.h"

#ifdef __APPLE__
#include <crt_externs.h>
#include <limits.h>

#include "mach-o/dyld.h"
#endif

namespace file_util {
fs::path get_user_home_dir() {
#ifdef _WIN32
  // NOTE - on older systems, this may case issues if it cannot be found!
  std::string home_dir = get_env("USERPROFILE");
  return fs::path(home_dir);
#else
  std::string home_dir = get_env("HOME");
  return fs::path(home_dir);
#endif
}

struct {
  bool initialized = false;
  fs::path path_to_data_folder;
  fs::path user_config_dir_override = "";
  // by default - if the config dir is overridden, we don't use the default save location
  bool use_overridden_config_dir_for_saves = true;
} g_file_path_info;

fs::path get_user_config_dir() {
  fs::path config_base_path;
#ifdef _WIN32
  auto config_base_dir = get_env("APPDATA");
  config_base_path = fs::path(config_base_dir);
#elif __linux
  // Docs - https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html
  // Prefer XDG_CONFIG_HOME if available
  auto config_base_dir = get_env("XDG_CONFIG_HOME");
  if (config_base_dir.empty()) {
    config_base_path = get_user_home_dir() / ".config";
  } else {
    config_base_path = fs::path(config_base_dir);
  }
#elif __APPLE__
  auto config_base_dir = get_env("HOME");
  config_base_path = fs::path(config_base_dir) / "Library" / "Application Support";
#elif defined(__SWITCH__)
  // This port has no HOME/XDG to anchor against, and leaving the base empty made the whole config
  // path relative ("OpenGOAL/jak1/settings/..."). That only resolved while the process's working
  // directory happened to be the NRO's own folder; a run started with any other cwd failed to
  // *read* display-settings.json, fell back to defaults, and then died on the save that follows
  // ("Unexpected exception occurred - couldn't open file ..."). Root it at the one absolute
  // location this build always has: the folder the NRO lives in.
  config_base_path = fs::path(get_current_executable_path()).parent_path();
#endif
  return config_base_path / "OpenGOAL";
}

fs::path get_user_settings_dir(GameVersion game_version) {
  auto game_version_name = game_version_names[game_version];
  auto config_dir = get_user_config_dir();
  if (!g_file_path_info.user_config_dir_override.empty()) {
    config_dir = g_file_path_info.user_config_dir_override / "OpenGOAL";
  }
  return config_dir / game_version_name / "settings";
}

fs::path get_user_memcard_dir(GameVersion game_version) {
  auto game_version_name = game_version_names[game_version];
  auto config_dir = get_user_config_dir();
  if (!g_file_path_info.user_config_dir_override.empty() &&
      g_file_path_info.use_overridden_config_dir_for_saves) {
    config_dir = g_file_path_info.user_config_dir_override / "OpenGOAL";
  }
  return config_dir / game_version_name / "saves";
}

fs::path get_user_screenshots_dir(GameVersion game_version) {
  auto game_version_name = game_version_names[game_version];
  auto config_dir = get_user_config_dir();
  if (!g_file_path_info.user_config_dir_override.empty()) {
    config_dir = g_file_path_info.user_config_dir_override / "OpenGOAL";
  }
  return config_dir / game_version_name / "screenshots";
}

fs::path get_user_misc_dir(GameVersion game_version) {
  auto game_version_name = game_version_names[game_version];
  auto config_dir = get_user_config_dir();
  if (!g_file_path_info.user_config_dir_override.empty()) {
    config_dir = g_file_path_info.user_config_dir_override / "OpenGOAL";
  }
  return config_dir / game_version_name / "misc";
}

fs::path get_user_features_dir(GameVersion game_version) {
  auto game_version_name = game_version_names[game_version];
  auto config_dir = get_user_config_dir();
  if (!g_file_path_info.user_config_dir_override.empty()) {
    config_dir = g_file_path_info.user_config_dir_override / "OpenGOAL";
  }
  auto path = config_dir / game_version_name / "features";
  file_util::create_dir_if_needed(path);
  return path;
}

fs::path g_iso_data_directory = "";

/*!
 * Get the path to the current executable.
 */
std::string get_current_executable_path() {
#ifdef _WIN32
  // NOTE - MAX_PATH is kinda wrong here as you can have a path longer than 260 in windows
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(NULL, path, MAX_PATH);
  std::string file_path = wide_string_to_utf8_string(path);
  if (file_path.rfind("\\\\?\\", 0) == 0) {
    return file_path.substr(4);
  }
  return file_path;
#elif __linux
  char buffer[FILENAME_MAX + 1];
  auto len = readlink("/proc/self/exe", buffer,
                      FILENAME_MAX);  // /proc/self acts like a "virtual folder" containing
  // information about the current process
  buffer[len] = '\0';
  return std::string(buffer);
#elif __APPLE__
  char buffer[PATH_MAX];
  uint32_t bufsize = sizeof(buffer);
  if (_NSGetExecutablePath(buffer, &bufsize) != 0) {
    lg::warn("Could not get executable path, trying with _NSGetArgv()[0] instead.");
    auto argv = *_NSGetArgv();
    return std::string(argv[0]);
  }
  return std::string(buffer);
#elif defined(__SWITCH__)
  // NRO homebrew has no real filesystem path for its own running image (it's loaded straight
  // into memory by the loader, not exec'd from a path argv[0] reliably carries) -- unlike every
  // other platform here, there's nothing to introspect. Just report the fixed SD card location
  // this game's data/ directory actually gets deployed to (the conventional
  // sdmc:/switch/<title>/ layout homebrew uses), so try_get_data_dir()'s
  // "next to the executable" logic still resolves to something real.
  return "sdmc:/switch/jak1/gk.nro";
#endif
}

std::optional<std::string> try_get_project_path_from_path(const std::string& path) {
  std::string::size_type pos =
      std::string(path).rfind("jak-project");  // Strip file path down to /jak-project/ directory
  if (pos == std::string::npos) {
    return {};
  }
  return std::string(path).substr(
      0, pos + 11);  // + 12 to include "/jak-project" in the returned filepath
}

/*!
 * See if the current executable is somewhere in jak-project/. If so, return the path to jak-project
 */
std::optional<std::string> try_get_jak_project_path() {
  return try_get_project_path_from_path(get_current_executable_path());
}

std::optional<fs::path> try_get_data_dir(bool skip_logs) {
  fs::path my_path = get_current_executable_path();
  if (!skip_logs) {
    lg::debug("Current executable directory - {}", my_path.string());
  }
  auto data_dir = my_path.parent_path() / "data";
  if (fs::exists(data_dir) && fs::is_directory(data_dir)) {
    return std::make_optional(data_dir);
  } else {
    return {};
  }
}

bool setup_project_path(std::optional<fs::path> project_path_override, bool skip_logs) {
  if (g_file_path_info.initialized) {
    return true;
  }

  if (project_path_override) {
    g_file_path_info.path_to_data_folder = fs::absolute(project_path_override.value());
    g_file_path_info.initialized = true;
    if (!skip_logs) {
      lg::debug("Using explicitly set project path: {}",
                g_file_path_info.path_to_data_folder.string());
    }
    return true;
  }

  auto data_path = try_get_data_dir(skip_logs);
  if (data_path) {
    g_file_path_info.path_to_data_folder = *data_path;
    g_file_path_info.initialized = true;
    if (!skip_logs) {
      lg::debug("Using data path: {}", data_path->string());
    }
    return true;
  }

  auto development_repo_path = try_get_jak_project_path();
  if (development_repo_path) {
    g_file_path_info.path_to_data_folder = *development_repo_path;
    g_file_path_info.initialized = true;
    if (!skip_logs) {
      lg::debug("Using development repo path: {}", *development_repo_path);
    }
    return true;
  }

  lg::error("Failed to initialize project path.");
  return false;
}

void override_user_config_dir(fs::path user_config_dir_override,
                              bool use_overridden_config_dir_for_saves) {
  g_file_path_info.user_config_dir_override = user_config_dir_override;
  g_file_path_info.use_overridden_config_dir_for_saves = use_overridden_config_dir_for_saves;
}

fs::path get_jak_project_dir() {
  ASSERT(g_file_path_info.initialized);
  return g_file_path_info.path_to_data_folder;
}

fs::path get_iso_dir_for_game(GameVersion game_version) {
  if (!g_iso_data_directory.empty()) {
    return g_iso_data_directory;
  }
  // Find the location based on the game version
  std::string expected_subdir = "jak1";
  if (game_version == GameVersion::Jak2) {
    expected_subdir = "jak2";
  } else if (game_version == GameVersion::Jak3) {
    expected_subdir = "jak3";
  }
  const auto temp_dir = get_jak_project_dir() / "iso_data" / expected_subdir;
  if (fs::exists(temp_dir)) {
    g_iso_data_directory = temp_dir;
  }
  return g_iso_data_directory;
}

void set_iso_data_dir(const fs::path& directory) {
  g_iso_data_directory = directory;
}

std::string get_file_path(const std::vector<std::string>& input) {
  // TODO - clean this behaviour up, it causes unexpected behaviour when working with files
  // the project path should be explicitly provided by whatever if needed
  // TEMP HACK
  // - if the provided path is absolute, don't add the project path
  //
  // "Absolute" has to include a device prefix. A name like "sdmc:/switch/jak1/OpenGOAL/jak1/
  // settings/pc-settings.gc" is as absolute as any POSIX path, but ghc::filesystem's
  // is_absolute() only looks for a leading '/', so the project directory was prepended to it and
  // the console's own trace shows the result on every run:
  //
  //   [file_util] mkdir: cannot resolve sdmc:/switch/jak1/data/sdmc:/switch/jak1/OpenGOAL/jak1/
  //                settings/pc-settings.gc
  //
  // The game-side file-stream write (sceOpen -> this function) is what builds that name, and the
  // write it belongs to is the pc-settings commit the auto-save process performs before every
  // save. It therefore failed every single time, leaving the settings file non-existent, and left
  // a throw-on-failure file write sitting in the middle of the save path. A path whose device
  // prefix precedes its first separator is already where it says it is.
  if (input.size() == 1) {
    const std::string& only = input.at(0);
    const auto colon = only.find(':');
    const auto slash = only.find('/');
    const bool has_device_prefix =
        colon != std::string::npos && colon != 0 &&
        (slash == std::string::npos || colon < slash);
    if (has_device_prefix || fs::path(only).is_absolute()) {
      return only;
    }
  }

  auto current_path = file_util::get_jak_project_dir();
  for (auto& str : input) {
    current_path /= str;
  }

  return current_path.string();
}

bool create_dir_if_needed(const fs::path& path) {
  // Error-code overloads: the throwing ones raise filesystem_error when the underlying stat/mkdir
  // fails, and on this port that failure is routine (see assert_file_exists above for what an
  // exception costs when it escapes into a thread that does not catch).
  std::error_code ec;
  if (!fs::is_directory(path, ec)) {
    std::error_code create_ec;
    fs::create_directories(path, create_ec);
    return true;
  }
  return false;
}

#ifdef __SWITCH__
// Defined further down with the rest of the Switch fs-service layer; declared here because the
// settings save path reaches them before that block.
static bool switch_create_directories(const std::string& name);
static bool switch_fs_exists(const std::string& name);
#endif

// TODO - explodes if the file path is invalid
bool create_dir_if_needed_for_file(const std::string& path) {
  return create_dir_if_needed_for_file(fs::path(path));
}

// TODO - explodes if the file path is invalid
bool create_dir_if_needed_for_file(const fs::path& path) {
#ifdef __SWITCH__
  // fs::create_directories() is ghc::filesystem -> newlib mkdir() -> devoptab, and on this build
  // it does not create anything under "sdmc:". Every caller ignores its return value, so the
  // failure was invisible: the settings write that followed threw "couldn't open file ..." out of
  // DisplayManager::init(), main() rethrew it and the process died on the title screen. Creating
  // the directory has to actually work, so go to the fs service directly.
  if (switch_create_directories(path.string())) {
    return true;
  }
  // Fallback for when the fs-service path refuses (it has a proven failure record on this card --
  // see switch_read_whole_file): newlib's mkdir, i.e. the same layer that successfully writes the
  // game log and reads the game data. Without a directory that exists, every write under it fails,
  // and the guest turns a failed settings write into a boot-time assert.
  {
    std::error_code ec;
    const fs::path parent = path.parent_path();
    if (!parent.empty()) {
      fs::create_directories(parent, ec);
      if (!ec) {
        return true;
      }
      switch_log_file("mkdir fallback failed for " + parent.string() + ": " + ec.message());
    }
  }
  return false;
#else
  return fs::create_directories(path.parent_path());
#endif
}

#ifdef __SWITCH__
// File I/O for the Switch port that goes straight to the fs service.
//
// Two separate faults led here, and only fixing the second one makes the port stable:
//
//  1. stdio's FILE* cannot cross threads on this toolchain. A stream created in, or used from,
//     anything but the thread that started the program faults inside newlib -- first seen in
//     fake_iso.cpp's fseek(), then in the graphics loader re-reading .fr3 files from its own
//     thread. Swapping FILE* for POSIX descriptors fixed that class of fault completely.
//  2. But `open()/close()` are NOT self-contained: newlib routes them through `devoptab_list`,
//     a global device table that libnx clears on process exit (__appExit() -> fsdev for "sdmc"
//     is torn down, devoptab_list[3] = NULL). Reading that table entry then dereferences NULL --
//     the Data Abort at offset 0x18 inside _close_r / _read_r. Any thread still doing file I/O
//     while anything in the process is exiting dies exactly this way, which is what killed the
//     console during the title screen (the sound bank / music load from the loader thread).
//
// So this port does not use the POSIX layer for its own file I/O at all: it resolves the fs
// device directly and talks to the FS service, which needs no devoptab entry on the hot path and
// is safe to call from any thread. Only ~10 files here, so the added verbosity is worth it.
static bool switch_resolve_fs(const std::string& name, FsFileSystem** fs, std::string& out_path) {
  char buf[FS_MAX_PATH];
  if (fsdevTranslatePath(name.c_str(), fs, buf) != 0 || !*fs) {
    return false;
  }
  out_path = buf;
  return true;
}

/*!
 * Open a file for reading, creating it, or truncating it, without touching devoptab_list.
 *
 * `why` (optional) receives a short "stage rc=0x..." description on failure. The caller puts it
 * into the exception message, because the sdmc:/gk_boot_log.txt sink has been observed arriving
 * on the card as a 0-byte file -- so the only channel that is known to work is the game's own
 * lg:: file logger, which prints ex.what(). Without this the crash line says "cannot be opened"
 * and gives no way to tell a missing file from a rejected path.
 */
static bool switch_fs_open(const std::string& name,
                           bool for_write,
                           FsFile* out,
                           std::string& resolved,
                           std::string* why = nullptr) {
  // A relative path is only correct while the process's working directory happens to be the
  // NRO's own folder, which a homebrew cannot rely on, and the engine still builds some paths by
  // hand (the user config dir used to be exactly this, and a cwd that did not match made reading
  // display-settings.json fail and the save that follows abort the process). So try the path as
  // given, then -- only if it is relative -- the same path anchored at the folder the NRO lives
  // in. "sdmc:"-style names already carry a device and are left alone.
  // Hold the FS-service lock for the whole operation: the open must never interleave with another
  // thread's request on the shared fsp-srv session (see switch_fs_lock()).
  std::lock_guard<std::recursive_mutex> fs_lock(switch_fs_lock());

  std::vector<std::string> candidates{name};
  if (name.find(':') == std::string::npos) {
    candidates.push_back(
        (fs::path(file_util::get_current_executable_path()).parent_path() / name).string());
  }

  Result last_error = 0;
  const char* last_stage = "resolve";
  for (const auto& candidate : candidates) {
    FsFileSystem* fs = nullptr;
    std::string path;
    if (!switch_resolve_fs(candidate, &fs, path)) {
      last_stage = "resolve";
      last_error = -1;
      continue;
    }
    // The fs API has no "create" or "truncate" open flag -- open modes are only Read/Write/Append
    // -- so both are done explicitly. Deleting first is what gives fopen("wb") semantics; if the
    // file does not exist yet, deleting fails harmlessly and the create below is what matters.
    if (for_write) {
      fsFsDeleteFile(fs, path.c_str());
      fsFsCreateFile(fs, path.c_str(), 0, 0);
    }
    // Retry on KernelError_InvalidMemoryState (0xD401). That code means the kernel refused this
    // request's IPC buffers, which is what happens when the shared fsp-srv session was busy with
    // another thread's request at that instant. It is transient and says nothing about the file --
    // which is why the card showed "exists=yes" for a path whose open had just been refused.
    // libnx retries this same code internally for fsFileRead(), so retrying here follows its own
    // precedent. Without it a single lost race kills the process: an unopenable shader throws
    // straight out of the graphics loop.
    constexpr Result kKernelInvalidMemoryState = 0xD401;
    Result rc = kKernelInvalidMemoryState;
    for (int attempt = 0; attempt < 8; ++attempt) {
      rc = fsFsOpenFile(fs, path.c_str(), for_write ? FsOpenMode_Write : FsOpenMode_Read, out);
      if (R_SUCCEEDED(rc) || rc != kKernelInvalidMemoryState) {
        break;
      }
      svcSleepThread(1000000ULL);  // 1 ms, then try again
    }
    if (R_SUCCEEDED(rc)) {
      resolved = candidate;
      return true;
    }
    last_stage = "open";
    last_error = rc;
  }

  char buf[96];
  snprintf(buf, sizeof(buf), "%s rc=0x%x", last_stage, (unsigned)last_error);
  if (why) {
    *why = buf;
  }
  switch_log_file("cannot open " + name + " (" + buf + ")");
  return false;
}

/*!
 * Does the path exist, according to the fs service?
 *
 * ghc::filesystem's fs::exists() (and status()) cannot be used here: they resolve through newlib's
 * stat()/devoptab, which reports a missing "sdmc:" file as present. That is not theoretical -- it
 * is why DisplaySettings::load_settings() logged "Loading display settings at ..." for a file that
 * was never there, then failed to read it. Same candidate rule as switch_fs_open() so relative
 * paths keep working.
 */
static bool switch_fs_exists(const std::string& name) {
  std::lock_guard<std::recursive_mutex> fs_lock(switch_fs_lock());
  std::vector<std::string> candidates{name};
  if (name.find(':') == std::string::npos) {
    candidates.push_back(
        (fs::path(file_util::get_current_executable_path()).parent_path() / name).string());
  }

  for (const auto& candidate : candidates) {
    FsFileSystem* fs = nullptr;
    std::string path;
    if (!switch_resolve_fs(candidate, &fs, path)) {
      continue;
    }
    FsDirEntryType type = (FsDirEntryType)-1;
    if (R_SUCCEEDED(fsFsGetEntryType(fs, path.c_str(), &type))) {
      return true;
    }
  }
  return false;
}

/*!
 * Log every entry of a directory, through the fs service.
 *
 * Called when a file cannot be opened, so that "is the file actually on the card?" is answered by
 * the console itself rather than by pulling the SD card over MTP. This matters because the
 * off-device tooling has already produced one wrong answer: a PowerShell listing keyed on
 * FolderItem.Name sees extension-less display names, which made a shaders folder holding 92 files
 * look like one holding 46, and made every .vert appear to carry its .frag sibling's size.
 *
 * Both channels are written -- the SD-card sink and lg:: -- because only one of them has ever
 * survived a given crash.
 */
static void switch_dump_directory(const std::string& dir) {
  std::lock_guard<std::recursive_mutex> fs_lock(switch_fs_lock());
  FsFileSystem* fs = nullptr;
  std::string resolved;
  if (!switch_resolve_fs(dir, &fs, resolved)) {
    lg::error("[dirlist] {}: cannot resolve", dir);
    switch_log_file("dirlist " + dir + ": cannot resolve\n");
    return;
  }
  FsDir d;
  if (R_FAILED(fsFsOpenDirectory(fs, resolved.c_str(),
                                 FsDirOpenMode_ReadFiles | FsDirOpenMode_ReadDirs, &d))) {
    lg::error("[dirlist] {}: cannot open directory", resolved);
    switch_log_file("dirlist " + resolved + ": cannot open directory\n");
    return;
  }

  // FsDirectoryEntry is ~784 bytes because name is FS_MAX_PATH, so the batch is heap-allocated
  // rather than put on a loader thread's stack.
  constexpr size_t kBatch = 32;
  // Capped. This used to list every entry, and the 10:13 run died inside that: 322 ISO entries,
  // two SD writes and a flush each, minutes of FS traffic -- and the process was killed by an
  // unrelated abort in the main thread before the listing even finished, which lost the one line
  // that says *which* open failed. 48 entries is enough to see a missing file and to recognise a
  // folder that is not the problem at all.
  constexpr s64 kMaxEntries = 48;
  std::vector<FsDirectoryEntry> batch(kBatch);
  s64 total = 0;
  bool capped = false;
  for (;;) {
    if (total >= kMaxEntries) {
      capped = true;
      break;
    }
    // fsDirRead() reports the number of entries it actually filled in *read, capped at kBatch;
    // the directory's overall size is what the separate fsDirGetEntryCount() is for.
    s64 read = 0;
    const Result rc = fsDirRead(&d, &read, kBatch, batch.data());
    if (R_FAILED(rc)) {
      char rc_buf[48];
      snprintf(rc_buf, sizeof(rc_buf), " rc=0x%x", (unsigned)rc);
      lg::error("[dirlist] {}: read failed after {} entries{}", resolved, (long long)total, rc_buf);
      switch_log_file("dirlist " + resolved + ": read failed after " + std::to_string(total) +
                      " entries" + rc_buf + "\n");
      break;
    }
    if (read <= 0) {
      break;
    }
    for (s64 i = 0; i < read && total < kMaxEntries; ++i, ++total) {
      const FsDirectoryEntry& e = batch[(size_t)i];
      const char* slash = e.type == FsDirEntryType_Dir ? "/" : "";
      lg::info("[dirlist] {}: {}{} ({} bytes)", resolved, e.name, slash, (long long)e.file_size);
      char line[FS_MAX_PATH + 64];
      snprintf(line, sizeof(line), "dirlist %s: %s%s (%lld bytes)\n", resolved.c_str(), e.name,
               slash, (long long)e.file_size);
      switch_log_file(line);
    }
  }
  fsDirClose(&d);
  lg::info("[dirlist] {}: listed {} entries{}", resolved, (long long)total,
           capped ? " (capped)" : "");
  switch_log_file("dirlist " + resolved + ": listed " + std::to_string(total) + " entries" +
                  (capped ? " (capped)\n" : "\n"));
}

/*!
 * Create every missing directory in the path's parent chain, via the fs service.
 *
 * fsFsCreateDirectory() fails when the level already exists and libnx exports no error-code
 * constant to test for that, so existence is checked with fsFsGetEntryType() instead and a failed
 * create is only an error if the entry still is not a directory afterwards. The function reports
 * true only when the parent chain really exists, which is the contract
 * create_dir_if_needed_for_file() callers assume.
 */
static bool switch_create_directories(const std::string& name) {
  std::lock_guard<std::recursive_mutex> fs_lock(switch_fs_lock());
  FsFileSystem* fs = nullptr;
  std::string resolved;
  if (!switch_resolve_fs(name, &fs, resolved)) {
    switch_log_file("mkdir: cannot resolve " + name);
    return false;
  }

  // Only the parent components matter -- the last one is the file being written.
  const size_t last_slash = resolved.find_last_of('/');
  if (last_slash == std::string::npos) {
    return true;  // a bare name in the device root: nothing to create
  }
  const std::string dir = resolved.substr(0, last_slash);

  // Walk "/switch", "/switch/jak1", ... creating each level in turn.
  std::string partial;
  size_t i = 0;
  while (i < dir.size()) {
    if (dir[i] == '/') {
      ++i;
      continue;
    }
    const size_t next = dir.find('/', i);
    const size_t end = (next == std::string::npos) ? dir.size() : next;
    partial += '/';
    partial += dir.substr(i, end - i);

    FsDirEntryType type = (FsDirEntryType)-1;
    if (R_FAILED(fsFsGetEntryType(fs, partial.c_str(), &type))) {
      const Result rc = fsFsCreateDirectory(fs, partial.c_str());
      if (R_FAILED(rc)) {
        FsDirEntryType check = (FsDirEntryType)-1;
        if (R_FAILED(fsFsGetEntryType(fs, partial.c_str(), &check)) ||
            check != FsDirEntryType_Dir) {
          char buf[96];
          snprintf(buf, sizeof(buf), " rc=0x%x", (unsigned)rc);
          switch_log_file("mkdir failed for " + partial + buf);
          return false;
        }
      }
    } else if (type != FsDirEntryType_Dir) {
      // A file sits where a directory has to go; the write cannot succeed.
      switch_log_file("mkdir blocked by non-directory " + partial);
      return false;
    }
    i = end;
  }

  fsFsCommit(fs);
  return true;
}

/*!
 * The file size, read from the open handle rather than by seeking to the end.
 */
// Largest single fsFileRead()/fsFileWrite() request this port makes.
//
// libnx hands the whole buffer to the kernel in one IPC request, and it does not accept an
// arbitrary size: a 67,584-byte bank write in one call comes back as a failure, leaves the file at
// 0 bytes, and reports nothing useful -- which is the shape every failed save had (the boot log's
// "[file_util] write ... failed (67584 bytes)", one line after "[MC] save_synch: writing 67584
// bytes to .../bank0.bin"). 16 KiB per request is what the port has used successfully for its
// other large transfers, so both directions go through this constant now.
constexpr u64 kSwitchFsChunkIo = 0x4000;

// Result codes are the only thing that distinguishes "the file is not there" from "the kernel
// refused this request's buffers", and neither is printable on its own.
static std::string switch_rc_str(Result rc) {
  char buf[32];
  snprintf(buf, sizeof(buf), "0x%x", (unsigned)rc);
  return buf;
}

static bool switch_fs_size(FsFile* f, u64& out) {
  s64 size = 0;
  if (R_FAILED(fsFileGetSize(f, &size)) || size < 0) {
    return false;
  }
  out = (u64)size;
  return true;
}

/*!
 * Fill a buffer from the file starting at offset 0.
 *
 * fsFileRead() is allowed to come back short and does on the SD card, so loop until the whole
 * block is in; a zero-byte return means the file ended early either way, which is an error here
 * because the caller already sized the file and reserved exactly that many bytes.
 */
static bool switch_fs_read_exact(FsFile* f, void* data, u64 len) {
  u8* p = (u8*)data;
  u64 done = 0;
  while (done < len) {
    u64 got = 0;
    const u64 want = std::min(kSwitchFsChunkIo, len - done);
    const Result rc = fsFileRead(f, (s64)done, p + done, want, 0, &got);
    if (R_FAILED(rc) || got == 0) {
      switch_log_file("read chunk failed at offset " + std::to_string(done) + " of " +
                      std::to_string(len) + " (" + switch_rc_str(rc) + ")");
      return false;
    }
    done += got;
  }
  return true;
}

/*!
 * Write a whole buffer at the given offset, looping over short writes.
 */
static bool switch_fs_write_all(FsFile* f, const void* data, u64 len, u64 off) {
  const u8* p = (const u8*)data;
  u64 done = 0;
  while (done < len) {
    const u64 n = std::min(kSwitchFsChunkIo, len - done);
    const Result rc = fsFileWrite(f, (s64)(off + done), p + done, n, FsWriteOption_None);
    if (R_FAILED(rc)) {
      // fsFileWrite() reports no byte count, so the failing request has to be named here: offset,
      // length and Result code. Without that a save failure is only "it did not work", which is
      // how a 67,584-byte write sat in a single call while every save produced a 0-byte bank.
      switch_log_file("write chunk failed at offset " + std::to_string(done) + " of " +
                      std::to_string(len) + " (" + std::to_string(n) + " bytes, rc=" +
                      switch_rc_str(rc) + ")");
      return false;
    }
    done += n;
  }
  // Like the old fsync(): the card is removable and the console can be powered off at any
  // moment, so push the data out before reporting success.
  return R_SUCCEEDED(fsFileFlush(f));
}

static std::vector<uint8_t> switch_read_whole_file(const fs::path& path) {
  // Held across open + size + read + close: the read travels on the same shared session, so it
  // must not interleave with another thread's request either.
  std::lock_guard<std::recursive_mutex> fs_lock(switch_fs_lock());
  const std::string name = path.string();

  // stdio first, deliberately.
  //
  // This is the one file-reading mechanism on this port with a proven record, and it is proven at
  // the exact moments the direct fs-service calls fail: the game's own log file
  // (sdmc:/switch/jak1/data/log/jak1.*.log) is written with newlib's fopen/fwrite/fflush from the
  // EE, IOP, DECI and loader threads, all run long -- and in those same runs
  // fsFsOpenFile(".../shaders/splash.vert") was coming back with 0xD401 on a file that
  // fsFsGetEntryType() found on the card. So the card, the filesystem and the fsp-srv session are
  // all fine; it is the fsFs* usage that is not. Reading a whole file is a self-contained
  // open-read-close on one thread, which is also the only shape newlib handles correctly on this
  // toolchain, so this path costs nothing in threading safety and sidesteps the failing one.
  //
  // The fs-service path below is kept as a fallback (and as the diagnostics: the directory dump and
  // the "cannot be opened" line still come from it), so a genuine deployment error still reports as
  // one instead of turning into a silent empty read.
  {
    std::vector<std::string> candidates{name};
    if (name.find(':') == std::string::npos) {
      candidates.push_back(
          (fs::path(file_util::get_current_executable_path()).parent_path() / name).string());
    }
    for (const auto& candidate : candidates) {
      // The staging buffer is heap-owned, and that is the fix for the one read that fails here.
      //
      // It used to be `uint8_t buf[64 * 1024]` -- a stack array -- and newlib reads a request
      // larger than its own FILE buffer straight into the caller's pointer, so the memory-card
      // bank read handed the OS a buffer belonging to this thread's stack. The console's trace:
      //
      //   [MC] file_is_present .../bank0.bin: size=67584 need=67584   <- fopen+fseek/ftell worked
      //   [file_util] read .../bank0.bin: stdio error, trying the fs service
      //   [file_util] cannot open .../bank0.bin (open rc=0xd401)
      //
      // 0xD401 decodes as KernelError_InvalidMemoryState -- "buffer is not usable for fs library"
      // -- and the same 67584 bytes written from a heap std::vector, by this same thread, through
      // these same stdio calls, works (that is how the save on this card got there). Buffers that
      // live on a GOAL thread's stack are inside the port's EE memory block, which the FS service
      // will not accept as a transfer buffer, so reads stage through the heap exactly like writes
      // already do. Small reads never showed this: newlib answers those from its own FILE buffer.
      //
      // Retried as well, because the same trace shows the fs-service open of a file that fopen
      // reads a moment later returning 0xD401 without ever succeeding -- a transient refusal is
      // worth surviving rather than turning into "this slot is empty".
      for (int attempt = 0; attempt < 3; ++attempt) {
        FILE* fp = fopen(candidate.c_str(), "rb");
        if (!fp) {
          break;
        }
        std::vector<uint8_t> data;
        std::vector<uint8_t> chunk(64 * 1024);  // heap, not a 64 KB stack array
        size_t got = 0;
        errno = 0;
        while ((got = fread(chunk.data(), 1, chunk.size(), fp)) > 0) {
          data.insert(data.end(), chunk.data(), chunk.data() + got);
        }
        const int read_errno = errno;
        const bool read_failed = ferror(fp) != 0;
        fclose(fp);
        if (!read_failed) {
          switch_log_file("read " + candidate + ": " + std::to_string(data.size()) +
                          " bytes (stdio)");
          return data;
        }
        switch_log_file("read " + candidate + ": stdio error after " +
                        std::to_string(data.size()) + " bytes, errno=" +
                        std::to_string(read_errno) + " (attempt " + std::to_string(attempt + 1) +
                        "/3)");
        svcSleepThread(20000000ULL);  // 20 ms before retrying the same read
      }
      switch_log_file("read " + candidate + ": stdio error, trying the fs service");
    }
  }

  FsFile f;
  std::string resolved;
  std::string why;
  if (!switch_fs_open(name, false, &f, resolved, &why)) {
    // Two very different situations, and the crash line used to be identical for both:
    //   exists=no  -> the file is genuinely not on the card (deployment problem)
    //   exists=yes -> it is there and the open was still refused (path/permission problem)
    // The second case is what "File .../direct_basic_textured.frag cannot be opened" looks like
    // when sd-tree.txt shows that exact file present at 2856 bytes.
    //
    // Enumerate the containing folder before throwing, so the log carries the console's own
    // view of what is on the card -- names, types and sizes -- instead of a listing produced
    // elsewhere and possibly through a name-mangling layer.
    // Report the failure *before* the directory dump. The dump can take a long time, and a run has
    // already died inside it (an unrelated abort in the main thread), losing the only line that
    // names the file and the reason. lg:: is the channel that has actually been readable on the
    // card every time; the boot-log sink above has arrived as a 0-byte file on every run so far.
    const std::string reason = "File " + name + " cannot be opened (" + why +
                               ", exists=" + (switch_fs_exists(name) ? "yes" : "no") + ")";
    lg::error("[file_util] {}", reason);
    switch_log_file(reason + "\n");
    switch_dump_directory(fs::path(name).parent_path().string());
    throw std::runtime_error(reason);
  }

  u64 len = 0;
  if (!switch_fs_size(&f, len)) {
    fsFileClose(&f);
    switch_log_file("cannot size " + name);
    throw std::runtime_error("File " + name + " cannot be sized");
  }
  if (len == 0) {
    fsFileClose(&f);
    switch_log_file("read " + name + ": empty");
    return {};
  }

  std::vector<uint8_t> data((size_t)len);
  if (!switch_fs_read_exact(&f, data.data(), data.size())) {
    fsFileClose(&f);
    switch_log_file("read " + name + " failed at " + std::to_string(data.size()) + " bytes");
    throw std::runtime_error("File " + name + " cannot be read");
  }
  fsFileClose(&f);
  switch_log_file("read " + name + ": " + std::to_string(data.size()) + " bytes");
  return data;
}

static void switch_write_whole_file(const fs::path& path, const void* data, size_t size) {
  std::lock_guard<std::recursive_mutex> fs_lock(switch_fs_lock());
  const std::string name = path.string();

  // stdio first, and that ordering is the point: this is a self-contained open/write/flush/close,
  // which is the one writing mechanism with a proven record on this card -- the game's own log is
  // written exactly this way, from the EE, IOP, DECI, loader and render threads, for an entire run
  // (300 KB per run in the logs pulled so far). The fs service is kept as the fallback below, but
  // it is the channel that failed the save: the bank write came back refused, the file was left at
  // 0 bytes, and the only trace was "[file_util] write ... failed (67584 bytes)" in the boot log.
  //
  // Writes are not equivalent to reads here (reads already prefer stdio for the same reason), and
  // a save that cannot be written is worse than a diagnostic that arrives by a different route.
  {
    FILE* fp = file_util::open_file(path, "wb");
    if (fp) {
      const size_t written = size ? fwrite(data, 1, size, fp) : 0;
      fflush(fp);
      const bool err = ferror(fp) != 0;
      fclose(fp);
      if (!err && written == size) {
        switch_log_file("write " + name + ": " + std::to_string(size) + " bytes (stdio)");
        return;
      }
      switch_log_file("write " + name + " stdio short: " + std::to_string(written) + " of " +
                      std::to_string(size) + " bytes, trying the fs service");
    } else {
      switch_log_file("write " + name + ": stdio open failed, trying the fs service");
    }
  }

  FsFile f;
  std::string resolved;
  if (!switch_fs_open(name, true, &f, resolved)) {
    throw std::runtime_error("couldn't open file " + name);
  }
  if (size == 0) {
    // nothing to write, just 'touch' the file
    fsFileClose(&f);
    return;
  }

  if (!switch_fs_write_all(&f, data, size, 0)) {
    fsFileClose(&f);
    switch_log_file("write " + name + " failed (" + std::to_string(size) + " bytes)");
    throw std::runtime_error("couldn't write file " + name);
  }
  fsFileClose(&f);
  // Commit so a file written from a non-main thread is durable, not just cached.
  FsFileSystem* fs = nullptr;
  if (switch_resolve_fs(name, &fs, resolved)) {
    fsFsCommit(fs);
  }
}
struct NativeFileHandle {
  FsFile file{};
  // Set only by the stdio fallback below. The ISO layer keeps one of these open across many reads
  // (fake_iso's open_fr / fs_read), and stdio cannot serve that reliably from the thread pool --
  // which is why the fs service is tried first -- but a fallback that works beats a nullptr that
  // asserts the process to death.
  FILE* fp = nullptr;
};

// The ISO layer is the busiest FS user on this port and the one that was still racing: every one
// of these is called from the fake-iso thread pool / EE-IOP threads (see fake_iso.cpp's open_fr and
// fs_read) at the same time the graphics loader is reading shaders, and they went to the fs service
// with no lock at all. That is the remaining source of "cannot be opened (open rc=0xd401, exists=no)"
// and of the 0-byte boot log -- the boot log's own open fails with the same code when an ISO read
// is in flight.
//
// switch_fs_open() already takes the lock internally; these take it as well so that a whole
// open/size/read/close operation -- the sequence the ISO layer performs -- is serialised against
// every other FS request rather than only its individual steps.
NativeFileHandle* native_open_read(const std::string& path) {
  std::lock_guard<std::recursive_mutex> fs_lock(switch_fs_lock());
  FsFile f;
  std::string resolved;
  if (switch_fs_open(path, false, &f, resolved)) {
    auto* handle = new NativeFileHandle();
    handle->file = f;
    return handle;
  }

  // The fs service refused a file that is on the card (GAME.CGO, 9,693,680 bytes, was the one that
  // proved it: fsFsOpenFile -> 0xD401 while the console's own listing showed the file at its
  // correct size). Retrying the same call is pointless -- eight attempts, 1 ms apart, all failed
  // earlier -- so fall back to stdio, the mechanism with a proven record here. The caller treats a
  // nullptr as fatal (fake_iso asserts), which is exactly why a fallback is worth having.
  FILE* fp = fopen(path.c_str(), "rb");
  if (fp) {
    auto* handle = new NativeFileHandle();
    handle->fp = fp;
    switch_log_file("native_open_read: stdio fallback used for " + path);
    return handle;
  }
  switch_log_file("native_open_read: both the fs service and stdio failed for " + path);
  return nullptr;
}

s64 native_file_size(NativeFileHandle* handle) {
  if (!handle) {
    return -1;
  }
  std::lock_guard<std::recursive_mutex> fs_lock(switch_fs_lock());
  if (handle->fp) {
    if (fseek(handle->fp, 0, SEEK_END) != 0) {
      return -1;
    }
    return (s64)ftell(handle->fp);
  }
  u64 size = 0;
  return switch_fs_size(&handle->file, size) ? (s64)size : -1;
}

bool native_read_at(NativeFileHandle* handle, u64 offset, void* dst, u64 len) {
  if (!handle) {
    return false;
  }
  // Held across the whole read, the way fsFileRead() is meant to be used: a multi-megabyte ISO read
  // is many libnx-level reads on one session. It also makes the stdio fallback safe: fseek+fread on
  // one FILE* from the thread pool is only a problem when two threads do it at once.
  std::lock_guard<std::recursive_mutex> fs_lock(switch_fs_lock());
  if (handle->fp) {
    if (fseek(handle->fp, (long)offset, SEEK_SET) != 0) {
      return false;
    }
    return fread(dst, 1, (size_t)len, handle->fp) == (size_t)len;
  }
  u8* p = (u8*)dst;
  u64 done = 0;
  while (done < len) {
    u64 got = 0;
    const Result rc =
        fsFileRead(&handle->file, (s64)(offset + done), p + done, len - done, 0, &got);
    if (R_FAILED(rc) || got == 0) {
      return false;
    }
    done += got;
  }
  return true;
}

void native_close(NativeFileHandle* handle) {
  if (!handle) {
    return;
  }
  std::lock_guard<std::recursive_mutex> fs_lock(switch_fs_lock());
  if (handle->fp) {
    fclose(handle->fp);
  } else {
    fsFileClose(&handle->file);
  }
  delete handle;
}
#else
// Desktop builds get the same interface on top of stdio. The callers only need "open once, read
// arbitrary ranges", so there is no reason for them to carry platform-specific code.
struct NativeFileHandle {
  FILE* fp = nullptr;
};

NativeFileHandle* native_open_read(const std::string& path) {
  FILE* fp = fopen(path.c_str(), "rb");
  if (!fp) {
    return nullptr;
  }
  auto* handle = new NativeFileHandle();
  handle->fp = fp;
  return handle;
}

s64 native_file_size(NativeFileHandle* handle) {
  if (!handle) {
    return -1;
  }
  if (fseek(handle->fp, 0, SEEK_END) != 0) {
    return -1;
  }
  const long size = ftell(handle->fp);
  rewind(handle->fp);
  return size;
}

bool native_read_at(NativeFileHandle* handle, u64 offset, void* dst, u64 len) {
  if (!handle) {
    return false;
  }
  if (fseek(handle->fp, (long)offset, SEEK_SET) != 0) {
    return false;
  }
  return fread(dst, 1, len, handle->fp) == len;
}

void native_close(NativeFileHandle* handle) {
  if (!handle) {
    return;
  }
  fclose(handle->fp);
  delete handle;
}
#endif

void write_binary_file(const fs::path& name, const void* data, size_t size) {
#ifdef __SWITCH__
  switch_write_whole_file(name, data, size);
  return;
#endif
  FILE* fp = file_util::open_file(name.string().c_str(), "wb");
  if (!fp) {
    throw std::runtime_error("couldn't open file " + name.string());
  }

  if (size == 0) {
    // nothing to write, just 'touch' the file
    fclose(fp);
    return;
  }

  if (fwrite(data, size, 1, fp) != 1) {
    fclose(fp);
    throw std::runtime_error("couldn't write file " + name.string());
  }

  fclose(fp);
}

void write_binary_file(const std::string& name, const void* data, size_t size) {
  write_binary_file(fs::path(name), data, size);
}

void write_rgba_png(const fs::path& name, void* data, int w, int h) {
  auto flags = 0;

  auto ok = fpng::fpng_encode_image_to_file(name.string().c_str(), data, w, h, 4, flags);

  if (!ok) {
    throw std::runtime_error("couldn't save png file " + name.string());
  }
}

void write_text_file(const std::string& file_name, const std::string& text) {
  write_text_file(fs::path(file_name), text);
}

void write_text_file(const fs::path& file_name, const std::string& text) {
#ifdef __SWITCH__
  const std::string with_newline = text + "\n";
  write_binary_file(file_name, with_newline.data(), with_newline.size());
  return;
#endif
  FILE* fp = file_util::open_file(file_name.string().c_str(), "w");
  if (!fp) {
    lg::error("Failed to fopen {}\n", file_name.string());
    throw std::runtime_error("Failed to open file");
  }
  fprintf(fp, "%s\n", text.c_str());
  fclose(fp);
}

bool try_write_text_file(const fs::path& file_name, const std::string& text) {
#ifdef __SWITCH__
  // The failure has to stay loud -- gk_boot_log.txt is the only channel that survives a crash on
  // this port -- but it must not propagate, because the settings saves run inside the boot path
  // and an exception from them reaches main()'s rethrow and kills the process.
  try {
    write_text_file(file_name, text);
    return true;
  } catch (const std::exception& e) {
    switch_log_file("write failed: " + file_name.string() + " (" + e.what() + ")");
    return false;
  }
#else
  try {
    write_text_file(file_name, text);
    return true;
  } catch (const std::exception& e) {
    lg::error("Failed to write {}: {}", file_name.string(), e.what());
    return false;
  }
#endif
}
std::vector<uint8_t> read_binary_file(const std::string& filename) {
  return read_binary_file(fs::path(filename));
}

std::vector<uint8_t> read_binary_file(const fs::path& path) {
#ifdef __SWITCH__
  // Deliberately before the status() checks below: those go through newlib's stat()/devoptab and
  // misreport "sdmc:" paths (a missing file can read as present, and a real one can fail the
  // regular-file test). switch_read_whole_file() asks the fs service and throws the real reason.
  return switch_read_whole_file(path);
#endif
  // make sure file exists and isn't a directory

  auto status = fs::status(path);

  if (!fs::exists(status)) {
    throw std::runtime_error(
        fmt::format("File {} cannot be opened: does not exist.", path.string()));
  }

  if (status.type() != fs::file_type::regular && status.type() != fs::file_type::symlink) {
    throw std::runtime_error(
        fmt::format("File {} cannot be opened: not a regular file or symlink.", path.string()));
  }

  auto fp = file_util::open_file(path.string().c_str(), "rb");
  if (!fp)
    throw std::runtime_error("File " + path.string() +
                             " cannot be opened: " + std::string(strerror(errno)));
  fseek(fp, 0, SEEK_END);
  auto len = ftell(fp);
  if (len == 0) {
    fclose(fp);
    return {};
  }
  rewind(fp);

  std::vector<uint8_t> data;
  data.resize(len);

  if (fread(data.data(), len, 1, fp) != 1) {
    fclose(fp);
    throw std::runtime_error("File " + path.string() + " cannot be read");
  }
  fclose(fp);

  return data;
}

std::string read_text_file(const fs::path& path) {
#ifdef __SWITCH__
  // ghc::filesystem's ifstream is stdio underneath, so it hits the same defect described above.
  const auto data = switch_read_whole_file(path);
  return std::string((const char*)data.data(), data.size());
#endif
  fs::ifstream file(path);
  if (!file.good()) {
    throw std::runtime_error("couldn't open " + path.string());
  }
  std::stringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

std::string read_text_file(const std::string& path) {
  return read_text_file(fs::path(path));
}

bool is_printable_char(char c) {
  return c >= ' ' && c <= '~';
}

std::string combine_path(const std::string& parent, const std::string& child) {
  return parent + "/" + child;
}

bool file_exists(const std::string& path) {
#ifdef __SWITCH__
  // ghc::filesystem reports missing "sdmc:" files as present (see switch_fs_exists()).
  return switch_fs_exists(path);
#else
  return fs::exists(path);
#endif
}

s64 file_size(const fs::path& path) {
#ifdef __SWITCH__
  // See the note in FileUtil.h: ghc::filesystem's file_size() throws when the file is missing,
  // and the first save into an empty memory-card slot asks about exactly such a file. Anything
  // that cannot be sized is reported as -1 so callers can compare instead of dying.
  auto* handle = native_open_read(path.string());
  if (!handle) {
    return -1;
  }
  const s64 len = native_file_size(handle);
  native_close(handle);
  return len;
#else
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec) {
    return -1;
  }
  return (s64)size;
#endif
}

std::string base_name(const std::string& filename) {
  size_t pos = 0;
  ASSERT(!filename.empty());
  for (size_t i = filename.size() - 1; i-- > 0;) {
    if (filename.at(i) == '/' || filename.at(i) == '\\') {
      pos = (i + 1);
      break;
    }
  }
  return filename.substr(pos);
}

std::string base_name_no_ext(const std::string& filename) {
  size_t pos = 0;
  ASSERT(!filename.empty());
  for (size_t i = filename.size() - 1; i-- > 0;) {
    if (filename.at(i) == '/' || filename.at(i) == '\\') {
      pos = (i + 1);
      break;
    }
  }
  std::string file_name = filename.substr(pos);
  return file_name.substr(0, file_name.find_last_of('.'));
  ;
}

std::string split_path_at(const fs::path& path, const std::vector<std::string>& folders) {
  std::string split_str = "";
  for (const auto& folder : folders) {
#ifdef _WIN32
    split_str += folder + "\\";
#else
    split_str += folder + "/";
#endif
  }
  const auto& path_str = path.string();
  return path_str.substr(path_str.find(split_str) + split_str.length());
}

std::string convert_to_unix_path_separators(const std::string& path) {
#ifdef _WIN32
  std::string copy = path;
  std::replace(copy.begin(), copy.end(), '\\', '/');
  return copy;
#else
  return path;
#endif
}

/*!
 * Convert an animation name to ISO name.
 * The animation name is a bunch of dash separated words.
 * The resulting ISO name has the same first two chars as the animation name, and one char from each
 * remaining word. Once there are no more words but remaining chars in the ISO name, the ith extra
 * char is the i+1 th char of the last word. A word ending in a number (or just a number) is turned
 * into the number. The word "resolution" becomes z. The word "accept" becomes y. The word "reject"
 * becomes n. Other words become the first char of the word. The result is uppercased and the file
 * extension is STR Examples (animation name and disc file name, not ISO name):
 *  green-sagecage-outro-beat-boss-enough-cells -> GRSOBBEC.STR
 *  swamp-tetherrock-swamprockexplode-4 -> SWTS4.STR
 *  minershort-resolution-1-orbs -> MIZ1ORBS.STR
 */
void ISONameFromAnimationName(char* dst, const char* src) {
  // The Animation Name is a bunch of words separated by dashes

  // copy first two chars of the first word exactly
  dst[0] = src[0];
  dst[1] = src[1];
  s32 i = 2;  // 2 chars added to dst.

  // skip ahead to the first dash (or \0 if there's no dashes)
  const char* src_ptr = src;
  while (*src_ptr && *src_ptr != '-') {
    src_ptr++;
  }

  // the points to the next dash (or \0 if there's none).
  const char* next_ptr = src_ptr;
  if (*src_ptr) {
    // loop over words (next_ptr points to dash before word, i counts chars in dest)
    while (src_ptr = next_ptr + 1, i < 8) {
      // scan next_ptr forward to next dash
      next_ptr = src_ptr;
      while (*next_ptr && *next_ptr != '-') {
        next_ptr++;
      }

      // there's no next word, so break (the current word will be handled there)
      if (!*next_ptr)
        break;

      // add a char for the current word:
      char char_to_add;
      if (next_ptr[-1] < '0' || next_ptr[-1] > '9') {
        // word doesn't end in a number.

        // some special case words map to special letters (likely to avoid animation name conflicts)
        if (next_ptr - src_ptr == 10 && !memcmp(src_ptr, "resolution", 10)) {
          // NOTE : jak 2 also allows "res" here but that doesn't work properly.
          char_to_add = 'z';
        } else if (next_ptr - src_ptr == 6 && !memcmp(src_ptr, "accept", 6)) {
          char_to_add = 'y';
        } else if (next_ptr - src_ptr == 6 && !memcmp(src_ptr, "reject", 6)) {
          char_to_add = 'n';
        } else if (next_ptr - src_ptr == 5 && !memcmp(src_ptr, "keira", 5)) {
          // NOTE : this was added in jak 2. it's safe to use in jak 1 since she was referred to as
          // "assistant" there
          char_to_add = 'i';
        } else {
          // not a special case, just take the first letter.
          char_to_add = *src_ptr;
        }
      } else {
        // the current word ends in a number, just use this number (I think usually the whole word
        // is just a number)
        char_to_add = next_ptr[-1];
      }

      dst[i++] = char_to_add;
    }

    // here we ran out of room in dest, or words in source.
    // if there's still room in dest and chars in source, just add them
    while (*src_ptr && (i < 8)) {
      dst[i] = *src_ptr;
      src_ptr++;
      i++;
    }
  }

  // pad with spaces (for ISO Name)
  while (i < 8) {
    dst[i++] = ' ';
  }

  // upper case
  for (i = 0; i < 8; i++) {
    if (dst[i] >= 'a' && dst[i] <= 'z') {
      dst[i] -= 0x20;
    }
  }

  // append file extension
  strcpy(dst + 8, "STR");
}

/*!
 * Convert file name to "ISO Name"
 * ISO names are upper case and 12 bytes long.
 * xxxxxxxxyyy0
 *
 * x - uppercase letter of file name, or space
 * y - uppercase letter of file extension, or space
 * 0 - null terminator (\0, not the character zero)
 */
void MakeISOName(char* dst, const char* src) {
  int i = 0;
  const char* src_ptr = src;
  char* dst_ptr = dst;

  // copy name and upper case
  while ((i < 8) && (*src_ptr) && (*src_ptr != '.')) {
    char c = *src_ptr;
    src_ptr++;
    if (('`' < c) && (c < '{')) {  // lower case
      c -= 0x20;
    }
    *dst_ptr = c;
    dst_ptr++;
    i++;
  }

  // pad out name with spaces
  while (i < 8) {
    *dst_ptr = ' ';
    dst_ptr++;
    i++;
  }

  // increment past period
  if (*src_ptr == '.')
    src_ptr++;

  // same for extension
  while (i < 11 && (*src_ptr)) {
    char c = *src_ptr;
    src_ptr++;
    if (('`' < c) && (c < '{')) {  // lower case
      c -= 0x20;
    }
    *dst_ptr = c;
    dst_ptr++;
    i++;
  }

  while (i < 11) {
    *dst_ptr = ' ';
    dst_ptr++;
    i++;
  }
  *dst_ptr = 0;
}

void assert_file_exists(const char* path, const char* error_message) {
#ifdef __SWITCH__
  // Neither fs::exists() nor the assert below may be used on this platform, and both were fatal:
  //
  //  * fs::exists() -> ghc::filesystem status() -> newlib's stat()/devoptab, which *throws*
  //    filesystem_error when the underlying stat fails with anything but ENOENT. This function is
  //    reached from inside an IOP RPC (fake_iso's FS_GetLength, via the RAMDISK load path) and
  //    nothing on that call chain catches: the exception unwound out of the coroutine thread into
  //    std::terminate -> abort -> __libnx_exit -> __appExit. That teardown then removed the sdmc
  //    devoptab entry under every other thread, and the loader thread's next stdout write died in
  //    _write_r with a Data Abort at offset 0x20 -- which is the crash the card reported, naming a
  //    line that had nothing to do with the actual failure.
  //  * aborting on "not found" does the same thing for a file that is simply missing, and on this
  //    card the fs service also refuses requests for files that *are* present (see the notes on
  //    switch_fs_open above), so a transient failure must not be fatal either.
  //
  // The fs service decides first; stdio -- the mechanism with a proven record here -- gets the last
  // word before the file is called missing. A missing file then fails the caller's own path: in
  // FS_GetLength that is a log line and a length of 0, which the RAMDISK RPC turns into a failed
  // load. Degrading there is the whole point: dying here takes the console with it.
  if (switch_process_dying().load(std::memory_order_relaxed)) {
    // Nothing here can be trusted while libnx is tearing down (fopen() walks the same devoptab
    // table that is being cleared), and the caller has its own failure path, so report nothing.
    return;
  }
  if (switch_fs_exists(path)) {
    return;
  }
  if (FILE* fp = fopen(path, "rb")) {
    fclose(fp);
    return;
  }
  const std::string reason = fmt::format("File {} was not found: {}", path, error_message);
  lg::error("[file_util] {}", reason);
  switch_log_file(reason);
#else
  if (!fs::exists(path)) {
    ASSERT_MSG(false, fmt::format("File {} was not found: {}", path, error_message));
  }
#endif
}

/*!
 * Check if the given DGO header (or entire file) is compressed.
 */
bool dgo_header_is_compressed(const std::vector<u8>& data) {
  const char compressed_header[] = "oZlB";
  bool is_compressed = true;
  for (int i = 0; i < 4; i++) {
    if (compressed_header[i] != data.at(i)) {
      is_compressed = false;
    }
  }
  return is_compressed;
}

/*!
 * Decompress a DGO. Resulting data will start at the DGO header.
 */
std::vector<u8> decompress_dgo(const std::vector<u8>& data_in) {
  constexpr int MAX_CHUNK_SIZE = 0x8000;
  BinaryReader compressed_reader(data_in);
  // seek past oZlB
  compressed_reader.ffwd(4);
  std::size_t decompressed_size = compressed_reader.read<uint32_t>();
  std::vector<uint8_t> decompressed_data;
  decompressed_data.resize(decompressed_size);
  size_t output_offset = 0;
  while (true) {
    // seek past alignment bytes and read the next chunk size
    uint32_t chunk_size = 0;
    while (!chunk_size) {
      chunk_size = compressed_reader.read<uint32_t>();
    }

    if (chunk_size < MAX_CHUNK_SIZE) {
      std::size_t bytes_written = 0;
      lzokay::EResult ok = lzokay::decompress(
          compressed_reader.here(), chunk_size, decompressed_data.data() + output_offset,
          decompressed_data.size() - output_offset, bytes_written);
      ASSERT(ok == lzokay::EResult::Success);
      compressed_reader.ffwd(chunk_size);
      output_offset += bytes_written;
    } else {
      // nope - sometimes chunk_size is bigger than MAX, but we should still use max.
      //        ASSERT(chunk_size == MAX_CHUNK_SIZE);
      memcpy(decompressed_data.data() + output_offset, compressed_reader.here(), MAX_CHUNK_SIZE);
      compressed_reader.ffwd(MAX_CHUNK_SIZE);
      output_offset += MAX_CHUNK_SIZE;
    }

    if (output_offset >= decompressed_size)
      break;
    while (compressed_reader.get_seek() % 4) {
      compressed_reader.ffwd(1);
    }
  }

  return decompressed_data;
}

FILE* open_file(const fs::path& path, const std::string& mode) {
#ifdef _WIN32
  return _wfopen(path.wstring().c_str(), std::wstring(mode.begin(), mode.end()).c_str());
#else
  return fopen(path.string().c_str(), mode.c_str());
#endif
}

std::vector<fs::path> find_files_in_dir(const fs::path& dir, const std::regex& pattern) {
  std::vector<fs::path> files = {};
  for (auto& p : fs::directory_iterator(dir)) {
    if (p.is_regular_file()) {
      if (std::regex_match(p.path().filename().string(), pattern)) {
        files.push_back(p.path());
      }
    }
  }
  return files;
}

std::vector<fs::path> find_files_recursively(const fs::path& base_dir, const std::regex& pattern) {
  std::vector<fs::path> files = {};
  if (!fs::exists(base_dir)) {
    return files;
  }
  for (auto& p : fs::recursive_directory_iterator(base_dir)) {
    if (p.is_regular_file()) {
      if (std::regex_match(p.path().filename().string(), pattern)) {
        files.push_back(p.path());
      }
    }
  }
  return files;
}

std::vector<fs::path> find_directories_in_dir(const fs::path& base_dir) {
  std::vector<fs::path> dirs = {};
  for (auto& p : fs::recursive_directory_iterator(base_dir)) {
    if (p.is_directory()) {
      dirs.push_back(p.path());
    }
  }
  return dirs;
}

std::vector<fs::path> sort_filepaths(const std::vector<fs::path>& paths, const bool aescending) {
  std::vector<std::string> paths_as_strings = {};
  for (const auto& path : paths) {
    paths_as_strings.push_back(path.string());
  }
  std::sort(paths_as_strings.begin(), paths_as_strings.end(),
            [aescending](const std::string& a, const std::string& b) {
              if (aescending) {
                return a < b;
              } else {
                return a > b;
              }
            });
  std::vector<fs::path> sorted_paths = {};
  for (const auto& path : paths_as_strings) {
    sorted_paths.push_back(fs::path(path));
  }
  return sorted_paths;
}

void copy_file(const fs::path& src, const fs::path& dst) {
  // Check that the src path exists
  if (!fs::exists(src)) {
    throw std::runtime_error(fmt::format("Cannot copy '{}', path does not exist", src.string()));
  }
  // Ensure the directory can be copied into
  if (!fs::exists(dst.parent_path()) && !create_dir_if_needed_for_file(dst)) {
    throw std::runtime_error(fmt::format(
        "Cannot copy '{}', couldn't make directory to copy into '{}'", src.string(), dst.string()));
  }
  fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
}

std::string make_screenshot_filepath(const GameVersion game_version, const std::string& name) {
  std::string file_name;
  if (name.empty()) {
    file_name = fmt::format("{}.png", str_util::current_local_timestamp_no_colons());
  } else {
    file_name = fmt::format("{}.png", name);
  }
  const auto file_path = get_user_screenshots_dir(game_version) / file_name;
  file_util::create_dir_if_needed_for_file(file_path);
  return file_path.string();
}

std::string get_majority_file_line_endings(const std::string& file_contents) {
  size_t lf_count = 0;
  size_t crlf_count = 0;

  for (size_t i = 0; i < file_contents.size(); ++i) {
    if (file_contents[i] == '\n') {
      if (i > 0 && file_contents[i - 1] == '\r') {
        crlf_count++;
      } else {
        lf_count++;
      }
    }
  }

  if (crlf_count > lf_count) {
    return "\r\n";
  }
  return "\n";
}

std::pair<int, std::string> get_majority_file_line_endings_and_count(
    const std::string& file_contents) {
  size_t lf_count = 0;
  size_t crlf_count = 0;

  for (size_t i = 0; i < file_contents.size(); ++i) {
    if (file_contents[i] == '\n') {
      if (i > 0 && file_contents[i - 1] == '\r') {
        crlf_count++;
      } else {
        lf_count++;
      }
    }
  }

  if (crlf_count > lf_count) {
    return {lf_count + crlf_count, "\r\n"};
  }
  return {lf_count + crlf_count, "\n"};
}

bool is_dir_in_dir(const fs::path& parent, const fs::path& child) {
  // Check if the parent path is a prefix of the child path
  return child.has_parent_path() && child.parent_path().lexically_relative(parent) == fs::path(".");
}

}  // namespace file_util
