#pragma once

/*!
 * @file stdout_sink.h
 * Non-failing stdout for the Switch port.
 *
 * This is where the "crash when saving" ended, so the reasoning is worth keeping:
 *
 *   * stdout is redirected to sdmc:/gk_stdout.txt (main.cpp). After early boot its writes start
 *     failing on this platform -- the file on the card is 735 bytes and stops at the last line
 *     printed before the game proper starts -- and the cause of that is not something this port
 *     has been able to fix (it is the same cross-thread/gamecard behaviour as the note in
 *     setup_logging(): file I/O on stdout from anything but the main thread, very early in boot).
 *   * Failing writes would be harmless if they were silent, but fmt::print() *throws*
 *     fmt::system_error when fewer bytes than asked reach the FILE. The message is literally
 *     "cannot write to file: I/O error" (fmt's fwrite_all).
 *   * The port has ~200 raw fmt::print sites, and one of them is mc_print() in the memory-card
 *     state machine -- i.e. the save path. So a save was:
 *
 *         mc-save -> mc_print -> fmt::print -> throw fmt::system_error -> uncaught
 *                 -> std::terminate -> abort -> the whole process is gone
 *
 *     gk_boot_log.txt recorded exactly that, one line after "[MC] requested save":
 *
 *         [fatal] std::terminate: cannot write to file: I/O error
 *
 * The fix is to stop pointing stdout at the card. It points at a memory stream
 * (open_memstream), whose writes cannot fail: nothing to lose, no fs-service round trip per line,
 * and no devoptab/thread interaction to trip over. The text is dumped to sdmc:/gk_stdout.txt on
 * demand -- at boot completion and on every fatal path -- so it is still readable after a run.
 *
 * The dump/reset pair is deliberately "leak the old stream": a thread that is in the middle of a
 * print when the swap happens writes into the retired buffer and loses a line, which is the right
 * trade against freeing a FILE another thread may be holding.
 */

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace switch_stdout_detail {
inline std::mutex& lock() {
  static std::mutex m;
  return m;
}
// open_memstream() owns these: it (re)writes both when the stream is flushed.
inline char*& buffer() {
  static char* b = nullptr;
  return b;
}
inline size_t& size() {
  static size_t s = 0;
  return s;
}
inline FILE*& stream() {
  static FILE* f = nullptr;
  return f;
}
inline bool& memory_mode() {
  static bool m = false;
  return m;
}
}  // namespace switch_stdout_detail

/*!
 * Point stdout at a memory stream. Returns false if that could not be set up, in which case the
 * caller is expected to fall back to the SD-card file as before.
 */
inline bool switch_stdout_install_to_memory() {
  using namespace switch_stdout_detail;
  std::lock_guard<std::mutex> guard(lock());
  FILE* mem = open_memstream(&buffer(), &size());
  if (!mem) {
    return false;
  }
  // Probe before committing: a memory stream must accept a write and a flush, and fmt/printf
  // failures here would be silent (and every later fmt::print would then throw).
  if (fwrite("", 1, 0, mem) != 0 || fflush(mem) != 0) {
    fclose(mem);
    return false;
  }
  stdout = mem;  // newlib: #define stdout _REENT_STDOUT(_REENT) -> assignable
  stream() = mem;
  memory_mode() = true;
  return true;
}

/*!
 * Replace the current memory stream with a fresh one, dropping what is in it. Only called after a
 * successful dump, so nothing is lost except lines written by a thread that was mid-print.
 */
inline void switch_stdout_reset() {
  using namespace switch_stdout_detail;
  std::lock_guard<std::mutex> guard(lock());
  if (!memory_mode()) {
    return;
  }
  FILE* fresh = open_memstream(&buffer(), &size());
  if (!fresh) {
    return;  // keep what we have; the next dump will pick it up
  }
  stdout = fresh;
  stream() = fresh;
}

/*!
 * Write everything printed so far to `path`, then start a fresh buffer.
 *
 * Safe to call from any thread and safe to call while other threads are printing (see the note at
 * the top about the retired stream). Uses ordinary stdio on purpose: the game log proves this path
 * works on this card from every thread for a whole run.
 */
inline void switch_stdout_dump_to_card(const char* path = "sdmc:/gk_stdout.txt") {
  using namespace switch_stdout_detail;
  size_t n = 0;
  {
    std::lock_guard<std::mutex> guard(lock());
    if (!memory_mode() || !stream()) {
      return;
    }
    fflush(stream());  // makes buffer()/size() current
    if (!buffer() || size() == 0) {
      return;
    }
    n = size();
    FILE* fp = fopen(path, "w");
    if (fp) {
      fwrite(buffer(), 1, n, fp);
      fflush(fp);
      fclose(fp);
    }
  }
  switch_stdout_reset();
}

/*!
 * Soft cap on the memory stream. Called from the ports' own periodic log sites; a run whose stdout
 * only carries an occasional warning never reaches this.
 */
inline void switch_stdout_trim_if_large(size_t limit = 4u * 1024u * 1024u) {
  using namespace switch_stdout_detail;
  size_t n = 0;
  {
    std::lock_guard<std::mutex> guard(lock());
    if (!memory_mode() || !stream()) {
      return;
    }
    fflush(stream());
    n = size();
  }
  if (n >= limit) {
    switch_stdout_dump_to_card();
  }
}
