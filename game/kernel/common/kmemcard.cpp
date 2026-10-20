/*!
 * @file kmemcard.cpp
 * Memory card interface. Very messy code. Most of it is commented out now, as we've switched away
 * from memory cards to just raw saves.
 *
 * Not checked carefully for differences in jak 2.
 */

#include "kmemcard.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"

#if defined(__SWITCH__)
#include "game/switch/boot_log.h"
#include "game/switch/stdout_sink.h"
#endif

#include "common/goal_constants.h"

#include "game/runtime.h"
#include "game/sce/sif_ee.h"
#include "game/sce/sif_ee_memcard.h"

#include "fmt/format.h"

#if defined(__SWITCH__)
// Left on for this port: the console has no debugger attached and the memory-card state machine
// is entirely silent otherwise, so a failed save shows up only as the game's own error dialog with
// no clue which step failed. Every mc_print line lands in sdmc:/gk_stdout.txt *and* in
// sdmc:/gk_boot_log.txt (see mc_print below) -- stdout stops being useful very early on this port,
// so the card trace is the one to read.
static constexpr bool memcard_debug = true;
#else
static constexpr bool memcard_debug = false;
#endif

/*!
 * Trace one memory-card step to the SD card, unconditionally and immediately.
 *
 * This is the diagnostics for the save/load state machine, and it exists because the interesting
 * failures here do not leave a crash report behind at all: an exception escaping a guest call, or
 * a libnx diag abort, ends the process in abort() with an on-screen error code and nothing on the
 * card. A step-by-step trace that is already flushed before the next step starts is what turns
 * "the console dies when the save menu is opened" into "it dies after update_card read bank2".
 *
 * It is deliberately *not* routed through mc_print (which goes to stdout), and not through lg::
 * either: switch_boot_log_event writes through the same stdio sink as the boot trace, under the FS
 * lock, and ignores the boot-only gate.
 */
#if defined(__SWITCH__)
#define MC_TRACE(...) switch_boot_log_event(fmt::format(__VA_ARGS__).c_str())
#else
#define MC_TRACE(...) ((void)0)
#endif

using McCallbackFunc = void (*)(s32);

McCallbackFunc callback;

static s32 language;
static MemoryCardOperation op;
// instead of two memory cards we just simulate the 4 save files (8 banks).
static MemoryCardFile mc_files[4];
// keep track of latest file selected. this is only used in an auto-save mode thats not used
static int mc_last_file = -1;

// a random value we will use as the memory card "handle" for the pc port, which has no memcards.
constexpr u32 PC_MEM_CARD_HANDLE = 0x6C616F67;

constexpr u32 MEM_CARD_MAGIC = 0x12345678;

struct McHeader {
  u32 save_count;
  u32 checksum;
  u32 magic;
  u8 preview_data[64];
  u8 data[944];
  u32 save_count2;
};
static_assert(sizeof(McHeader) == 0x400, "McHeader size");

static McHeader header;

// these are the return value for sceMcGetInfo.
static s32 p1, p2, p3, p4;
using namespace ee;

template <typename... Args>
void mc_print(const std::string& str, Args&&... args) {
  if (!memcard_debug) {
    return;
  }
  {
#if defined(__SWITCH__)
    std::string line = fmt::format(fmt::runtime(str), std::forward<Args>(args)...);
    // The card trace is the one that survives a crash (see MC_TRACE).
    switch_boot_log_event(("[MC] " + line + '\n').c_str());
    // ...and the game log, through lg, *not* fmt::print: fmt::print to this port's stdout threw
    // fmt::system_error ("cannot write to file: I/O error") the moment its write stopped landing,
    // and this call site is the save path -- that exception was the crash. lg's stdout branch is
    // off on Switch (see setup_logging), so lg only writes the file.
    if (!line.empty() && line.back() == '\n') {
      line.pop_back();
    }
    lg::print("[MC] {}", line);
    // stdout is a memory stream here; keep it from growing without bound on long runs.
    switch_stdout_trim_if_large();
#else
    if (!str.empty() && str.back() == '\n') {
      fmt::print(fmt::runtime(str), std::forward<Args>(args)...);
    } else {
      fmt::print(fmt::runtime(str + '\n'), std::forward<Args>(args)...);
    }
#endif
  }
}

const char* filename_jak1[12] = {
    "BASCUS-97124AYBABTU!",           "BASCUS-97124AYBABTU!/icon.sys",
    "BASCUS-97124AYBABTU!/icon.ico",  "BASCUS-97124AYBABTU!/BASCUS-97124AYBABTU!",
    "BASCUS-97124AYBABTU!/bank0.bin", "BASCUS-97124AYBABTU!/bank1.bin",
    "BASCUS-97124AYBABTU!/bank2.bin", "BASCUS-97124AYBABTU!/bank3.bin",
    "BASCUS-97124AYBABTU!/bank4.bin", "BASCUS-97124AYBABTU!/bank5.bin",
    "BASCUS-97124AYBABTU!/bank6.bin", "BASCUS-97124AYBABTU!/bank7.bin"};

const char* filename_jak2[12] = {
    "BASCUS-97265AYBABTU!",           "BASCUS-97265AYBABTU!/icon.sys",
    "BASCUS-97265AYBABTU!/icon.ico",  "BASCUS-97265AYBABTU!/BASCUS-97265AYBABTU!",
    "BASCUS-97265AYBABTU!/bank0.bin", "BASCUS-97265AYBABTU!/bank1.bin",
    "BASCUS-97265AYBABTU!/bank2.bin", "BASCUS-97265AYBABTU!/bank3.bin",
    "BASCUS-97265AYBABTU!/bank4.bin", "BASCUS-97265AYBABTU!/bank5.bin",
    "BASCUS-97265AYBABTU!/bank6.bin", "BASCUS-97265AYBABTU!/bank7.bin"};

const char* filename_jak3[12] = {
    "BASCUS-97330AYBABTU!",           "BASCUS-97330AYBABTU!/icon.sys",
    "BASCUS-97330AYBABTU!/icon.ico",  "BASCUS-97330AYBABTU!/BASCUS-97330AYBABTU!",
    "BASCUS-97330AYBABTU!/bank0.bin", "BASCUS-97330AYBABTU!/bank1.bin",
    "BASCUS-97330AYBABTU!/bank2.bin", "BASCUS-97330AYBABTU!/bank3.bin",
    "BASCUS-97330AYBABTU!/bank4.bin", "BASCUS-97330AYBABTU!/bank5.bin",
    "BASCUS-97330AYBABTU!/bank6.bin", "BASCUS-97330AYBABTU!/bank7.bin"};

const char* mc_get_filename_no_dir(GameVersion version, int ndx) {
  const char** filenames = nullptr;
  switch (version) {
    case GameVersion::Jak1:
      filenames = filename_jak1;
      break;
    case GameVersion::Jak2:
      filenames = filename_jak2;
      break;
    case GameVersion::Jak3:
      filenames = filename_jak3;
      break;
  }
  return filenames[ndx];
}

inline fs::path mc_get_filename(GameVersion version, int ndx) {
  return file_util::get_user_memcard_dir(version) / mc_get_filename_no_dir(version, ndx);
}

int mc_get_total_bank_size(GameVersion) {
  return BANK_SIZE[g_game_version] + sizeof(McHeader) * 2;
}

void kmemcard_init_globals() {
  // next = 0;
  language = 0;
  op = {};
  // mc[0] = {};
  // mc[1] = {};
  mc_files[0] = {};
  mc_files[1] = {};
  mc_files[2] = {};
  mc_files[3] = {};
  callback = nullptr;
  p1 = 0;
  p2 = 0;
  p3 = 0;
  p4 = 0;
  // memset(&dirent, 0, sizeof(sceMcTblGetDir));
  memset(&header, 0, sizeof(McHeader));
}

/*!
 * A questionable checksum used on memory card data.
 */
u32 mc_checksum(Ptr<u8> data, s32 size) {
  if (size < 0) {
    size += 3;
  }

  u32 result = 0;
  u32* data_u32 = (u32*)data.c();
  for (s32 i = 0; i < size / 4; i++) {
    result = result << 1 ^ (s32)result >> 0x1f ^ data_u32[i] ^ MEM_CARD_MAGIC;
  }

  return result ^ 0xedd1e666;
}

/*!
 * One probed save slot on the Switch's SD card.
 *
 * Probing the card from GOAL code turned out to be the pause-menu hang: the save menu's
 * (respond-common) runs (mc-get-slot-info) every frame it is open, MC_get_status() served every
 * one of those calls with a full rescan of all four slots, and each rescan was six card-opening
 * round trips plus a 67 KB read per present bank -- the game log of the build28 run shows the
 * whole probe cycle repeating with a ~10-line period for hundreds of consecutive frames, each
 * frame dispatching in 130-177 ms. The card is not going to be rotated underneath us (there is no
 * physical card to swap; saves only appear through this process's own writes), so the probe
 * results can be cached and refreshed rarely, like the console's own card-state polling did.
 */
// Forward declaration: the cache code below reads 1 KB headers through this helper, whose full
// definition lives after the state machine (it needs MC_TRACE and file_util, and this file's
// include order has them set up by then).
static bool pc_read_bank_header(const fs::path& path, McHeader& out);

struct McSlotCacheEntry {
  bool probed = false;             // have we ever looked at this slot?
  bool present = false;            // bank0 exists and is full-size
  bool aux_present = false;        // the alternate bank (bank0+1) exists and is full-size
  u32 save_count = 0;
  u32 last_saved_bank = 0;
  McHeader header{};               // of the most recent of the two banks
};

struct McSlotCache {
  McSlotCacheEntry slots[4];
  Timer since_probe;               // time since the last full (re)probe
  bool dirty = true;               // must refresh before the next answer is served
};
static McSlotCache mc_cache;

// The refresh interval. Long enough that a probe cycle (roughly 0.3-0.4 s of card I/O) can never
// happen every frame; short enough that a save written from the other side of a restart is seen
// within a second. Saves push immediate invalidation anyway (mc_invalidate_cache), so this only
// governs how often the menu re-reads for its own sake.
constexpr double MC_PROBE_INTERVAL_SECONDS = 1.0;

/*!
 * Drop the cache and force the next mc-cache request to re-probe the card.
 *
 * Call after every save and load: those change the card's contents (new bank, bumped save count)
 * and the state machine reads files-is-present/headers right after them, which must see the new
 * state, not the cached one.
 */
static void mc_invalidate_cache() {
  if (mc_cache.dirty) {
    return;  // already invalidated; no need to reset the timer again
  }
  mc_cache.dirty = true;
  MC_TRACE("[MC] cache invalidated\n");
}

/*!
 * Full re-probe of the four save slots, filling the cache.
 *
 * This is the old per-frame work, run at most once per second (and only when somebody asks):
 * for each slot, open bank0, size it, read its header if sized right, then the same for the
 * alternate bank. Missing banks are "fopen failed" -- one open attempt, no read, and today's
 * earlier rate-limited switch_log_file keeps them from flooding the card trace.
 */
static void mc_refresh_cache() {
  for (int file = 0; file < 4; file++) {
    auto& e = mc_cache.slots[file];
    e = {};  // probed=false, everything else zeroed
    e.probed = true;

    const auto bank0_path = mc_get_filename(g_game_version, 4 + file * 2);
    const s64 size0 = file_util::file_size(bank0_path);
    if (size0 < (s64)mc_get_total_bank_size(g_game_version)) {
      // bank0 missing (or short): the slot is empty, and bank1 is irrelevant.
      MC_TRACE("[MC] probe slot {}: bank0 size={} need={}\n", file, size0,
               mc_get_total_bank_size(g_game_version));
      continue;
    }

    McHeader h0{};
    if (!pc_read_bank_header(bank0_path, h0)) {
      // The size check passed but the read failed: treat as empty, same "believe the read" rule
      // the old pc_update_card had.
      MC_TRACE("[MC] probe slot {}: bank0 header read failed\n", file);
      continue;
    }
    e.present = true;
    e.save_count = h0.save_count;
    e.header = h0;
    e.last_saved_bank = 0;

    const auto bank1_path = mc_get_filename(g_game_version, 1 + 4 + file * 2);
    const s64 size1 = file_util::file_size(bank1_path);
    if (size1 >= (s64)mc_get_total_bank_size(g_game_version)) {
      McHeader h1{};
      if (pc_read_bank_header(bank1_path, h1)) {
        e.aux_present = true;
        if (h1.save_count > h0.save_count) {
          // use most recent bank here.
          e.save_count = h1.save_count;
          e.header = h1;
          e.last_saved_bank = 1;
        }
      }
    }
    MC_TRACE("[MC] probe slot {}: present=1 save_count={} bank={}\n", file, e.save_count,
             e.last_saved_bank);
  }
  mc_cache.since_probe.start();
  mc_cache.dirty = false;
}

/*!
 * Is the cache good enough to answer a request right now?
 *
 * Anything that says "the card changed" calls mc_invalidate_cache() instead of waiting for the
 * timer, so the timer only governs the menu's own periodic re-probe.
 */
static void mc_ensure_cache_fresh() {
  if (mc_cache.dirty || !mc_cache.since_probe.getSeconds() ||
      mc_cache.since_probe.getSeconds() >= MC_PROBE_INTERVAL_SECONDS) {
    mc_refresh_cache();
  }
}

/*!
 * PC port function that returns whether a given bank ID's file exists or not.
 *
 * Served from the cache (mc_ensure_cache_fresh), not from the card: callers are the frame-rate
 * hot path (MC_load before a save/load, MC_run's reload check) as well as the state machine, and
 * the old version opened at least one file on the card every call.
 */
bool file_is_present(int id, int bank = 0) {
  // Guest-supplied index: the save path already refuses >= 4 at its entry, but MC_run's LOAD
  // branch lands here with whatever file_idx the guest passed to MC_load, and a -1 that got
  // through (0xffffffff, the "most recent file" the state machine is supposed to resolve
  // itself) used to index straight off the end -- first of the filename table, and since
  // build29 of this cache. Treat any out-of-range slot as "not present".
  if (id < 0 || id > 3) {
    MC_TRACE("[MC] file_is_present: slot {} out of range, refused\n", id);
    return false;
  }
  mc_ensure_cache_fresh();
  const auto& e = mc_cache.slots[id];
  const bool present = bank == 0 ? e.present : e.aux_present;
  MC_TRACE("[MC] file_is_present slot={} bank={}: {} (cached)\n", id, bank, present ? "yes" : "no");
  return present;
}

/*!
 * PC port function to set memcard info. We don't use a memory card, instead just the raw savefiles.
 *
 * This is the function the save/load menu runs when it opens, to fill in the four slots and their
 * preview icons -- the background autosave never calls it. Every failure here (a missing bank, a
 * short bank, a read that throws) now degrades to "this slot is empty" and leaves a trace on the
 * card instead of ending the process.
 *
 * Almost free now: it copies the probe cache into the mc_files table the guest reads. The actual
 * card work happens in mc_refresh_cache(), at most once per second no matter how often this is
 * called -- and the menu calls it every frame (progress.gc's respond-common).
 */
void pc_update_card() {
  u32 highest_save_count = 0;
  mc_last_file = -1;
  mc_ensure_cache_fresh();
  for (s32 file = 0; file < 4; file++) {
    const auto& e = mc_cache.slots[file];
    mc_files[file].present = e.present;
    mc_files[file].most_recent_save_count = e.save_count;
    mc_files[file].last_saved_bank = e.last_saved_bank;
    memcpy(mc_files[file].data, e.header.preview_data, 64);

    // Upstream computed last_file here and it is *not* optional: the save state machine resolves
    // the file to save over from it -- (get-card) does (set! (-> self which) (-> self info
    // last-file)) and bails out with (mc-status-code no-last) when it is -1. Leaving it at -1 makes
    // every 'save-last request fail even when a save exists on the card (the card trace showed
    // "get_status: present=1000 last_file=-1" right after a successful save), so the game reports
    // the save as failed while the bank file sits on the card, complete and valid.
    if (mc_files[file].present && mc_files[file].most_recent_save_count > highest_save_count) {
      mc_last_file = file;
      highest_save_count = mc_files[file].most_recent_save_count;
    }
  }
}

// The Switch memory card I/O also goes through file_util, and therefore through the same
// FS-service path as everything else -- see the long note in common/util/FileUtil.cpp. It must
// not use the POSIX open/read/write/close layer: newlib routes those through devoptab_list,
// which is torn down when any part of the process starts exiting, so a save or a load racing an
// exit dies on a null device entry. file_util's helpers read/write the whole file in one call,
// which is what both callers here want anyway (a bank is written or read wholesale).
static bool mc_write_whole_file(const fs::path& path, const void* data, size_t len) {
  try {
    file_util::write_binary_file(path, data, len);
    return true;
  } catch (const std::exception& e) {
    fmt::print("[MC] write {} failed: {}\n", path.string(), e.what());
    return false;
  }
}

static bool mc_read_whole_file(const fs::path& path, void* data, size_t len) {
  try {
    const auto contents = file_util::read_binary_file(path);
    if (contents.size() < len) {
      fmt::print("[MC] read {} short: {} of {} bytes\n", path.string(), contents.size(), len);
      return false;
    }
    memcpy(data, contents.data(), len);
    return true;
  } catch (const std::exception& e) {
    fmt::print("[MC] read {} failed: {}\n", path.string(), e.what());
    return false;
  }
}

/*!
 * PC port function to save a file. This does the whole saving at once, synchronously.
 */
void pc_game_save_synch() {
  Timer mc_timer;
  mc_timer.start();
  MC_TRACE("[MC] save_synch: enter slot={}\n", op.param2);

  // The file index comes from the guest. The menu passes 0..3, but the game also uses -1 to mean
  // "the most recent file" (see the save state machine: it resolves that to (-> info last-file)
  // before it ever reaches mc-save), and a -1 that got through would arrive here as 0xffffffff.
  // It indexes both mc_files[] and the bank filename table, so refuse it the way a real card would
  // -- report the error and clear the op -- rather than walking off the end of the host heap.
  if (op.param2 >= 4) {
    MC_TRACE("[MC] save_synch: bad file index {}, refused", op.param2);
    op.operation = MemoryCardOperationKind::NO_OP;
    op.result = McStatusCode::NO_FILE;
    return;
  }

  pc_update_card();
  auto path = mc_get_filename(g_game_version, 0);
  file_util::create_dir_if_needed_for_file(path.string());

  // cd_reprobe_save //
  if (!file_is_present(op.param2)) {
    mc_print("reprobe save: first time!");
    // first time saving!
    p2 = 0;  // save count 0
    p4 = 0;  // first bank for file
  } else {
    p2 = mc_files[op.param2].most_recent_save_count + 1;  // increment save count
    p4 = mc_files[op.param2].last_saved_bank ^ 1;         // use the other bank
  }

  // reserve 0 as "I never saved" and use 1 instead.
  if (p2 == 0) {
    p2 = 1;
  }

  // file*2 + p4 is the bank (2 banks per file, p4 is 0 or 1 to select the bank)
  // 4 is the first bank file
  mc_print("open {} for saving", mc_get_filename_no_dir(g_game_version, op.param2 * 2 + 4 + p4));
  auto save_path = mc_get_filename(g_game_version, op.param2 * 2 + 4 + p4);
  file_util::create_dir_if_needed_for_file(save_path.string());

  // cb_openedsave //
  mc_print("save file opened, writing header...");
  memset(&header, 0, sizeof(McHeader));
  header.save_count = p2;
  header.checksum = mc_checksum(op.data_ptr, BANK_SIZE[g_game_version]);
  header.magic = MEM_CARD_MAGIC;
  header.save_count2 = p2;
  memcpy(header.preview_data, op.data_ptr2.c(), 64);

  // TEMP SAVE-PATH DIAGNOSTIC (build27) -- removed in build29.
  //
  // It ran on every save and dumped 96+ bytes through MC_TRACE. The save itself was proven good
  // (every build28 run wrote a complete, checksummed bank that loaded back -- and the build26
  // crash it was hunting turned out to be the stdout I/O path, not the save data), so there is
  // nothing left it was chasing. The menu-crash-on-first-present-slot it was built for is now
  // covered by the cache (this file's top note) by keeping the preview-copy loop short.

  // Assemble the whole bank in ordinary heap memory and hand it to file_util in one call.
  //
  // The bank itself lives in emulated EE main memory (the jitCreate() block on this port), and
  // the file used to be built up through raw write() calls. Both are gone now: the copy below
  // keeps EE pages out of the I/O path, and file_util is the single place that knows how to
  // reach the card safely (see its comment -- the POSIX layer underneath is not usable here
  // because newlib resolves it through devoptab_list, which dies with any process exit).
  const size_t bank_size = BANK_SIZE[g_game_version];
  std::vector<u8> image(sizeof(McHeader) + bank_size + sizeof(McHeader));
  memcpy(image.data(), &header, sizeof(McHeader));
  memcpy(image.data() + sizeof(McHeader), op.data_ptr.c(), bank_size);
  memcpy(image.data() + sizeof(McHeader) + bank_size, &header, sizeof(McHeader));

  // cb_saveddata + cb_savedfooter //
  mc_print("save file writing main data ({} bytes)", image.size());
  MC_TRACE("[MC] save_synch: writing {} bytes to {}\n", image.size(), save_path.string());
  if (mc_write_whole_file(save_path, image.data(), image.size())) {
    MC_TRACE("[MC] save_synch: wrote ok\n");
    // cb_closedsave //
    mc_print("All done with saving!!");
    op.operation = MemoryCardOperationKind::NO_OP;
    op.result = McStatusCode::OK;
    mc_files[op.param2].present = 1;
    mc_files[op.param2].most_recent_save_count = p2;
    mc_files[op.param2].last_saved_bank = p4;
    memcpy(mc_files[op.param2].data, op.data_ptr2.c(), 64);
    mc_last_file = op.param2;
    // The card's contents just changed (new bank on disk, new save count). The in-memory mc_files
    // update above covers the immediate consumers, and the cache is invalidated so the next probe
    // (whether from the 1 s timer or an explicit request) re-reads the card instead of serving the
    // pre-save picture.
    mc_invalidate_cache();
    fmt::print("[MC] save wrote {} bytes to {}\n", image.size(), save_path.string());
  } else {
    fmt::print("[MC] save FAILED writing {} bytes to {}\n", image.size(), save_path.string());
    op.operation = MemoryCardOperationKind::NO_OP;
    op.result = McStatusCode::INTERNAL_ERROR;
    mc_invalidate_cache();
  }

  mc_print("[MC] synchronous save took {:.2f}ms\n", mc_timer.getMs());
}

/*!
 * Read one bank out of a save file into emulated EE memory.
 *
 * Takes the path of the bank to read rather than an open descriptor: the whole file is pulled in
 * by file_util (the only layer allowed to touch the card), so there is nothing to keep open
 * across the asynchronous steps in the middle of this function -- which is also why this is now
 * re-entrant per bank without leaking a descriptor on every path that used to call close().
 *
 * @param bank_path     file to read (one bank of the save)
 * @param required_size bytes that must be present, or the read is rejected
 * @returns true if the bank was read and copied into EE memory
 */
static bool pc_game_load_one_bank(const fs::path& bank_path, size_t required_size) {
  // cb_openedload //
  mc_print("reading save file {}...", bank_path.string());
  // Mirror of the save path: pull the file into ordinary heap memory first and copy it into
  // emulated EE memory afterwards, rather than asking the fs service to write straight into the
  // jitCreate() block (which is what made saving fail).
  std::vector<u8> staging(required_size);
  if (mc_read_whole_file(bank_path, staging.data(), required_size)) {
    memcpy(op.data_ptr.c() + p2 * required_size, staging.data(), required_size);
    // cb_readload + cb_closedload //
    return true;
  }
  return false;
}

void pc_game_load_open_file(bool opened) {
  if (opened) {
    // cb_openedload //
    size_t read_size = mc_get_total_bank_size(g_game_version);
    auto bank_path = mc_get_filename(g_game_version, op.param2 * 2 + 4 + p2);
    if (pc_game_load_one_bank(bank_path, read_size)) {
      // cb_closedload //
      // added : check if aux bank exists
      // file_util::file_exists, not fs::exists: see file_is_present -- ghc::filesystem answers
      // exists() from an sdmc: stat through the global devoptab and gets it wrong.
      if (p2 < 1 && file_util::file_exists(
                       mc_get_filename(g_game_version, op.param2 * 2 + 4 + p2 + 1).string())) {
        p2++;
        mc_print("reading next save bank {}",
                 mc_get_filename_no_dir(g_game_version, op.param2 * 2 + 4 + p2));
        pc_game_load_open_file(true);
      } else {
          // let's verify the data.
          McHeader* headers[2];
          McHeader* footers[2];
          bool ok[2];

          headers[0] = (McHeader*)(op.data_ptr.c());
          footers[0] = (McHeader*)(op.data_ptr.c() + sizeof(McHeader) + BANK_SIZE[g_game_version]);
          headers[1] = (McHeader*)(op.data_ptr.c() + mc_get_total_bank_size(g_game_version));
          footers[1] = (McHeader*)(op.data_ptr.c() + mc_get_total_bank_size(g_game_version) +
                                   sizeof(McHeader) + BANK_SIZE[g_game_version]);
          // static_assert(mc_get_total_bank_size(g_game_version) * 2 == 0x21000, "save layout");
          ok[0] = true;
          ok[1] = p2 == 1;

          for (int idx = 0; idx < 2; idx++) {
            u32 expected_save_count = headers[idx]->save_count;
            if (headers[idx]->save_count2 == expected_save_count &&
                footers[idx]->save_count == expected_save_count &&
                footers[idx]->save_count2 == expected_save_count) {
              // save count is okay!
              if (headers[idx]->magic == MEM_CARD_MAGIC && footers[idx]->magic == MEM_CARD_MAGIC) {
                // magic numbers okay!
                if (headers[idx]->checksum == footers[idx]->checksum) {
                  // checksum
                  auto expected_checksum = headers[idx]->checksum;
                  if (mc_checksum(make_u8_ptr(headers[idx] + 1), BANK_SIZE[g_game_version]) !=
                      expected_checksum) {
                    mc_print("failed checksum");
                    ok[idx] = false;
                  }
                } else {
                  mc_print("corrupted checksum");
                  ok[idx] = false;
                }
              } else {
                mc_print("bad magic");
                ok[idx] = false;
              }
            } else {
              mc_print("bad save count");
              ok[idx] = false;
            }
          }

          mc_print("checking loaded banks");

          //
          if (!ok[0] && !ok[1]) {
            // no good data.
            if (headers[0]->save_count == 0 && headers[0]->checksum == 0 &&
                headers[0]->magic == 0 && headers[0]->save_count2 == 0 &&
                headers[1]->save_count == 0 && headers[1]->checksum == 0 &&
                headers[1]->magic == 0 && headers[1]->save_count2 == 0) {
              // this is a fresh file that you tried to load from...
              mc_print("new game result");
              op.operation = MemoryCardOperationKind::NO_OP;
              op.result = McStatusCode::NEW_GAME;
              mc_last_file = op.param2;
              mc_invalidate_cache();
            } else {
              mc_print("corrupted data");
              op.operation = MemoryCardOperationKind::NO_OP;
              op.result = McStatusCode::READ_ERROR;
              mc_invalidate_cache();
            }
          } else {
            // pick the bank
            int bank = 0;

            if (!ok[0] || !ok[1]) {
              if (ok[1]) {
                bank = 1;
              }
            } else {
              bank = headers[0]->save_count <= headers[1]->save_count;
            }

            mc_print(fmt::format("loading bank {}", bank));
            u32 current_save_count = headers[bank]->save_count;
            memmove(
                op.data_ptr.c(),
                op.data_ptr.c() + bank * mc_get_total_bank_size(g_game_version) + sizeof(McHeader),
                BANK_SIZE[g_game_version]);
            mc_last_file = op.param2;
            mc_files[op.param2].most_recent_save_count = current_save_count;
            mc_files[op.param2].last_saved_bank = bank;
            op.operation = MemoryCardOperationKind::NO_OP;
            op.result = McStatusCode::OK;
            mc_print("load succeeded");
            mc_invalidate_cache();
        }
      }
    } else {
      op.operation = MemoryCardOperationKind::NO_OP;
      op.result = McStatusCode::INTERNAL_ERROR;
    }
  } else {
    op.operation = MemoryCardOperationKind::NO_OP;
    op.result = McStatusCode::INTERNAL_ERROR;
  }
}

/*!
 * PC port function to load a file. This does the whole loading at once, synchronously.
 */
void pc_game_load_synch() {
  Timer mc_timer;
  mc_timer.start();
  // Same guest-supplied-index guard as the save path (pc_game_save_synch): op.param2 arrives
  // straight from the guest, and a -1/0xffffffff that escaped the state machine's own resolution
  // would index both the filename table and the databank layout off their ends.
  if (op.param2 >= 4) {
    MC_TRACE("[MC] load_synch: bad file index {}, refused\n", op.param2);
    op.operation = MemoryCardOperationKind::NO_OP;
    op.result = McStatusCode::NO_FILE;
    return;
  }
  pc_update_card();

  // cb_reprobe_load //
  p2 = 0;
  mc_print("opening save file {}", mc_get_filename_no_dir(g_game_version, op.param2 * 2 + 4));

  auto path = mc_get_filename(g_game_version, op.param2 * 2 + 4);
  // A save file that is not there (or is still the truncated 1024-byte one an earlier build
  // wrote) is reported rather than opened -- there is nothing to open a descriptor for.
  MC_TRACE("[MC] load_synch: file {} exists={}\n", path.string(), file_util::file_exists(path.string()));
  pc_game_load_open_file(file_util::file_exists(path.string()));

  mc_print("synchronous load took {:.2f}ms\n", mc_timer.getMs());
}

/*!
 * Run the Memory Card state machine.  This is called once per frame in GOAL.
 * It:
 *  - does nothing if there is an in-progress memory card operation
 *  - if async memory card functions are done, runs their callbacks
 *  - if there is a requested operation, starts running sony functions.
 *  - if there is none of the above, and unknown cards, finds out about them.
 *  - every now and then, recheck cards.
 */
void MC_run() {
  // if we have an in-progress operation, it will have set a callback.
  if (callback) {
    s32 sony_cmd, sony_status;
    // check the status
    s32 status = sceMcSync(1, &sony_cmd, &sony_status);
    McCallbackFunc callback_for_sync = callback;
    if (status == sceMcExecRun) {
      // busy, return.
      return;
    }

    if (status == sceMcExecFinish) {
      // sony function is done. do the callback.
      callback = nullptr;
      (*callback_for_sync)(sony_status);
    } else {
      // sony function is done, but failed.
      callback = nullptr;
      (*callback_for_sync)(0);
    }

    if (callback) {
      // if we got another callback, it means there's another op started by the prev callback.
      // and this case, we want to wait for that operation to finish.
      return;
    }
  }

  // if we got here, there is no in-progress sony function. So start the next one, if we should
  if (op.operation == MemoryCardOperationKind::FORMAT) {
    // format memory card. Not used in PC port, so lets move on.
    return;
  } else if (op.operation == MemoryCardOperationKind::UNFORMAT) {
    // unformat memory card.
    return;
  } else if (op.operation == MemoryCardOperationKind::CREATE_FILE) {
    // create the game file.
    // there's no cards, keep in mind.
    return;
  } else if (op.operation == MemoryCardOperationKind::SAVE) {
    // write game save.
    // there's no cards, keep in mind.
    MC_TRACE("[MC] run: SAVE slot={} retry={}\n", op.param2, op.retry_count);
    pc_game_save_synch();
    // allow some number of errors.
    op.retry_count--;
    if (op.retry_count == 0) {
      op.operation = MemoryCardOperationKind::NO_OP;
      op.result = McStatusCode::INTERNAL_ERROR;
    }
  } else if (op.operation == MemoryCardOperationKind::LOAD) {
    // load game save.
    // potato.
    MC_TRACE("[MC] run: LOAD slot={} retry={}\n", op.param2, op.retry_count);
    if (!file_is_present(op.param2)) {
      // tried to load, but there's no save data in the file.
      op.operation = MemoryCardOperationKind::NO_OP;
      op.result = McStatusCode::NO_MEMORY;
    } else {
      pc_game_load_synch();
      op.retry_count--;
      if (op.retry_count == 0) {
        op.operation = MemoryCardOperationKind::NO_OP;
        op.result = McStatusCode::INTERNAL_ERROR;
      }
    }
  }
}

/////////////////////////
// Memory Card Functions
/////////////////////////

// These functions are called from GOAL to start memory card operations.

/*!
 * Set the language or something.
 * Why is this a memory card func?
 */
void MC_set_language(s32 l) {
  printf("Language set to %d\n", l);
  language = l;
}

/*!
 * Set the current memory card operation to FORMAT the given card.
 * Doesn't do anything in the port because we don't use memory cards.
 */
u64 MC_format(s32 /*card_idx*/) {
  return u64(McStatusCode::OK);
  // u64 can_add = op.operation == MemoryCardOperationKind::NO_OP;
  // mc_print("requested format");
  // if (can_add) {
  //  mc_print("setting op to format");
  //  op.operation = MemoryCardOperationKind::FORMAT;
  //  op.result = McStatusCode::BUSY;
  //  op.retry_count = 100;
  //  op.param = card_idx;
  //}
  // return can_add;
}

/*!
 * Set the current memory card operation to UNFORMAT the given card.
 * You get the idea.
 */
u64 MC_unformat(s32 /*card_idx*/) {
  return u64(McStatusCode::OK);
  // u64 can_add = op.operation == MemoryCardOperationKind::NO_OP;
  // mc_print("requested unformat");
  // if (can_add) {
  //  mc_print("setting op to unformat");
  //  op.operation = MemoryCardOperationKind::UNFORMAT;
  //  op.result = McStatusCode::BUSY;
  //  op.retry_count = 100;
  //  op.param = card_idx;
  //}
  // return can_add;
}

/*!
 * Set the current memory card operation to create the save file.
 * The data I believe is just an empty buffer used as temporary storage.
 */
u64 MC_createfile(s32 /*param*/, Ptr<u8> /*data*/) {
  return u64(McStatusCode::OK);
  // u64 can_add = op.operation == MemoryCardOperationKind::NO_OP;
  // mc_print("requested createfile");
  // if (can_add) {
  //  mc_print("setting op to create file");
  //  op.operation = MemoryCardOperationKind::CREATE_FILE;
  //  op.result = McStatusCode::BUSY;
  //  op.retry_count = 100;
  //  op.param = param;
  //  op.data_ptr = data;
  //}
  // return can_add;
}

/*!
 * Set the current operation to SAVE.
 * The "summary data" is data that will be used when previewing save files (number of orbs etc)
 * TODO put synchronous call here
 */
u64 MC_save(s32 card_idx, s32 file_idx, Ptr<u8> save_data, Ptr<u8> save_summary_data) {
  mc_print("requested save");
  u64 can_add = op.operation == MemoryCardOperationKind::NO_OP;
  if (can_add) {
    mc_print("setting op to save");
    op.operation = MemoryCardOperationKind::SAVE;
    op.result = McStatusCode::BUSY;
    op.retry_count = 100;
    op.param = card_idx;
    op.param2 = file_idx;
    op.data_ptr = save_data;
    op.data_ptr2 = save_summary_data;
  }
  return can_add;
}

/*!
 * Set the current operation to LOAD.
 * TODO put synchronous call here
 */
u64 MC_load(s32 card_idx, s32 file_idx, Ptr<u8> data) {
  mc_print("requested load");
  u64 can_add = op.operation == MemoryCardOperationKind::NO_OP;
  if (can_add) {
    mc_print("setting op to load");
    op.operation = MemoryCardOperationKind::LOAD;
    op.result = McStatusCode::BUSY;
    op.retry_count = 100;
    op.param = card_idx;
    op.param2 = file_idx;
    op.data_ptr = data;
  }
  return can_add;
}

/*!
 * Some sort of test function for memory card stuff.
 * This is exported as a GOAL function, but nothing calls it.
 */
void MC_makefile(s32 port, s32 size) {
  sceMcMkdir(port, 0, "/BASCUS-00000XXXXXXXX");
  // wait for operation to complete
  s32 cmd, result, fd;
  sceMcSync(0, &cmd, &result);

  if (result == sceMcResSucceed || result == sceMcResNoEntry) {
    // it worked, or the folder already exists...

    // open file
    sceMcOpen(port, 0, "/BASCUS-00000XXXXXXXX/BASCUS-00000XXXXXXXX", SCE_CREAT | SCE_WRONLY);
    sceMcSync(0, &cmd, &fd);

    if (result < 0) {
      printf("Can't open file on memcard [%d]\n", result);
    } else {
      // write some random crap into the memory card.
      sceMcWrite(fd, Ptr<u8>(0x1000000).c(), size);
      sceMcSync(0, &cmd, &result);
      if (result != size) {
        printf("Only written %d bytes\n", result);
      }
      sceMcClose(fd);
      sceMcSync(0, &cmd, &result);
    }
  } else {
    printf("Can\'t create garbage folder [%d]\n", result);
  }
}

/*!
 * Get the result of the currently executing (or most recently executed) command
 */
u32 MC_check_result() {
  return (u32)op.result;
}

/*!
 * Update the info for the given slot.
 * You can call this at any time.
 * The slot includes the four save slots (8 banks), and a few other files.
 */
void MC_get_status(s32 /*slot*/, Ptr<mc_slot_info> info) {
  // slot is ignored, so you'll get the same thing regardless of what slot you pick

  info->handle = 0;
  info->known = 0;
  info->formatted = 0;
  info->initted = 0;
  for (s32 i = 0; i < 4; i++) {
    info->files[i].present = 0;
  }
  info->last_file = 0xffffffff;
  info->mem_required = SAVE_SIZE[g_game_version];
  info->mem_actual = 0;

  pc_update_card();
  info->known = 1;
  info->handle = PC_MEM_CARD_HANDLE;
  info->formatted = 1;
  info->mem_actual = SAVE_SIZE[g_game_version];  // idk TODO does this matter?
  info->initted = 1;
  // copy over the preview data.
  for (s32 file = 0; file < 4; file++) {
    info->files[file].present = mc_files[file].present;
    for (s32 i = 0; i < 64; i++) {  // actually a loop over u32's
      info->files[file].data[i] = mc_files[file].data[i];
    }
  }
  info->last_file = mc_last_file;
  MC_TRACE("[MC] get_status: present={}{}{}{} last_file={} mem_actual={}\n",
           info->files[0].present, info->files[1].present, info->files[2].present,
           info->files[3].present, info->last_file, info->mem_actual);
}

/*!
 * Read one bank's 1 KB header, or false if the file is missing, unreadable, or too short.
 *
 * Defined here, after its first use in mc_refresh_cache() (which needs the declaration above the
 * cache code, and this file's macros are already all set up by then).
 *
 * Never throws, and that is the point: this runs inside a guest call, and an exception escaping a
 * guest call ends the process in abort() with no crash report.
 *
 * Gets whole-file reads for granted in the first years of the port: the probe used to pull the
 * entire 67 KB bank to look at 1 KB of header. native_read_at with a 1 KB buffer buys that back,
 * and it does not stage the file through a heap vector the way read_binary_file did.
 */
static bool pc_read_bank_header(const fs::path& path, McHeader& out) {
  auto* handle = file_util::native_open_read(path.string());
  if (!handle) {
    // Missing: one open attempt, and today's FileUtil rate-limits the log line.
    return false;
  }
  const s64 file_len = file_util::native_file_size(handle);
  if (file_len < (s64)sizeof(McHeader)) {
    file_util::native_close(handle);
    MC_TRACE("[MC] read header {} short: {} bytes\n", path.string(), file_len);
    return false;
  }
  // The staging buffer must live on the HEAP, not this thread's C++ stack.
  //
  // On this port the guest call runs on the EE thread whose stack is inside the emulated EE main
  // memory block (the jitCreate() region), and the FS service refuses transfer buffers from that
  // region -- the exact failure switch_read_whole_file() already documented for the 64 KB stack
  // staging buffer it once used ("buffer is not usable for fs library", 0xD401) and fixed by
  // staging through a heap vector. The evidence for it being the same here: every probe cycle in
  // the build29 card log failed at the (read) step, 77/77, while the identical-size bank write
  // from a heap std::vector succeeded every time, and the one cycle whose fs-service open was
  // refused (0xd401) and therefore fell back to stdio/fread is the only probe that ever read a
  // header successfully -- fread through newlib does not hand the raw address to the kernel.
  const std::vector<u8> buffer(sizeof(McHeader));
  const bool ok = file_util::native_read_at(handle, 0, const_cast<u8*>(buffer.data()), sizeof(McHeader));
  file_util::native_close(handle);
  if (!ok) {
    MC_TRACE("[MC] read header {} failed (read)\n", path.string());
    return false;
  }
  memcpy(&out, buffer.data(), sizeof(McHeader));
  return true;
}
