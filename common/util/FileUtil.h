#pragma once

/*!
 * @file FileUtil.h
 * Utility functions for reading and writing files.
 */

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#endif

#include "third-party/filesystem.hpp"

#ifdef _WIN32
#undef FALSE
#endif

#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/versions/versions.h"

namespace fs = ghc::filesystem;

namespace file_util {
fs::path get_user_home_dir();
fs::path get_user_config_dir();
fs::path get_user_settings_dir(GameVersion game_version);
fs::path get_user_memcard_dir(GameVersion game_version);
fs::path get_user_screenshots_dir(GameVersion game_version);
fs::path get_user_misc_dir(GameVersion game_version);
fs::path get_user_features_dir(GameVersion game_version);
fs::path get_jak_project_dir();
fs::path get_iso_dir_for_game(GameVersion game_version);
void set_iso_data_dir(const fs::path& directory);

bool create_dir_if_needed(const fs::path& path);
bool create_dir_if_needed_for_file(const std::string& path);
bool create_dir_if_needed_for_file(const fs::path& path);
std::string get_current_executable_path();
std::optional<std::string> try_get_project_path_from_path(const std::string& path);
bool setup_project_path(std::optional<fs::path> project_path_override, bool skip_logs = false);
void override_user_config_dir(fs::path user_config_dir_override,
                              bool use_overridden_config_dir_for_saves);
std::string get_file_path(const std::vector<std::string>& path);
void write_binary_file(const std::string& name, const void* data, size_t size);
void write_binary_file(const fs::path& name, const void* data, size_t size);
void write_rgba_png(const fs::path& name, void* data, int w, int h);
void write_text_file(const std::string& file_name, const std::string& text);
void write_text_file(const fs::path& file_name, const std::string& text);

/*!
 * Write a text file, returning false instead of throwing when it cannot be written.
 *
 * The settings saves run during boot, and an exception out of one of them used to escape all the
 * way out of main() -- which rethrows, so the process died on the title screen with nothing but
 * "Unexpected exception occurred" in the log. A save that cannot be written deserves a loud line
 * on the SD card, not a dead process, so callers that must not abort use this. The failure is
 * still reported: on Switch to gk_boot_log.txt, elsewhere through lg.
 */
bool try_write_text_file(const fs::path& file_name, const std::string& text);
std::vector<uint8_t> read_binary_file(const std::string& filename);
std::vector<uint8_t> read_binary_file(const fs::path& filename);

/*!
 * Block-oriented read handle for the Switch port.
 *
 * The engine reads files in offset-sized chunks (the ISO/overlord loader pulls one sector run at a
 * time out of a file it keeps open), so read_binary_file()'s "read the whole thing" shape is the
 * wrong tool there. That code used to keep a POSIX descriptor open across threads instead -- which
 * is exactly what this port cannot do, for the reasons documented at the top of FileUtil.cpp:
 * newlib resolves open/read/close through the global devoptab_list, and that table is torn down
 * when any part of the process begins exiting, so a read racing a shutdown dereferences a null
 * device entry.
 *
 * These calls go straight to the FS service, need no device table, and are safe from any thread.
 * The handle is opaque so this header does not have to pull in the platform's headers.
 */
struct NativeFileHandle;
NativeFileHandle* native_open_read(const std::string& path);
// File size, or -1 if the handle is bad.
s64 native_file_size(NativeFileHandle* handle);
// Reads exactly `len` bytes from `offset`. false on any error or short read.
bool native_read_at(NativeFileHandle* handle, u64 offset, void* dst, u64 len);
void native_close(NativeFileHandle* handle);
std::string read_text_file(const std::string& path);
std::string read_text_file(const fs::path& path);
bool is_printable_char(char c);
std::string combine_path(const std::string& parent, const std::string& child);
bool file_exists(const std::string& path);

/*!
 * Size of a file in bytes, or -1 when it cannot be determined. Never throws.
 *
 * The memory-card code used to guard its reads with `fs::exists(p) || fs::file_size(p) < n`, and on
 * this port that guard is worse than useless in one specific direction: ghc::filesystem answers
 * exists() from a stat through the global devoptab (see file_exists()), and its file_size() throws
 * ghc::filesystem::filesystem_error when that stat fails. The first save into an empty slot is
 * exactly that case -- the bank file does not exist yet -- and an exception out of a guest call is
 * an uncaught exception, which ends the process in abort() with no crash report at all.
 *
 * Callers here want a number they can compare, so this answers -1 for "missing or unreadable" and
 * never throws. On Switch it asks the FS service (native_open_read/native_file_size), which is the
 * only layer with a reliable record on sdmc: for this port.
 */
s64 file_size(const fs::path& path);
std::string base_name(const std::string& filename);
std::string base_name_no_ext(const std::string& filename);
std::string split_path_at(const fs::path& path, const std::vector<std::string>& folders);
std::string convert_to_unix_path_separators(const std::string& path);
void MakeISOName(char* dst, const char* src);
void ISONameFromAnimationName(char* dst, const char* src);
void assert_file_exists(const char* path, const char* error_message);
bool dgo_header_is_compressed(const std::vector<u8>& data);
std::vector<u8> decompress_dgo(const std::vector<u8>& data_in);
FILE* open_file(const fs::path& path, const std::string& mode);
std::vector<fs::path> find_files_in_dir(const fs::path& dir, const std::regex& pattern);
std::vector<fs::path> find_files_recursively(const fs::path& base_dir, const std::regex& pattern);
std::vector<fs::path> find_directories_in_dir(const fs::path& base_dir);
std::vector<fs::path> sort_filepaths(const std::vector<fs::path>& paths, const bool aescending);
/// Will overwrite the destination if it exists
void copy_file(const fs::path& src, const fs::path& dst);
std::string make_screenshot_filepath(const GameVersion game_version, const std::string& name = "");
std::string get_majority_file_line_endings(const std::string& file_contents);
std::pair<int, std::string> get_majority_file_line_endings_and_count(
    const std::string& file_contents);
bool is_dir_in_dir(const fs::path& parent, const fs::path& child);
}  // namespace file_util
