#pragma once

/*!
 * @file boot_log.h
 * Shared SD-card trace sink for the Switch port.
 *
 * Each translation unit used to open/write/close sdmc:/gk_boot_log.txt per message. newlib's
 * fsdev layer isn't thread-safe, and the EE/IOP/DECI/main threads all trace concurrently, so
 * that raced its internal state; it also cost three emulated fsp-srv round-trips per line.
 * One buffer, one fd, guarded by a mutex, and the file is rewritten from the buffer's own
 * write offset -- never from a size query (see switch_boot_log_write).
 *
 * Tracing is boot-only. Several of these call sites live in the renderer and would otherwise
 * run every frame, doing guest file I/O from the render thread -- which is where the printf
 * path was faulting once the game loop started. switch_boot_log_finish() latches it off.
 *
 * The sink uses newlib's stdio (fopen/fwrite/fflush/fclose) under the FS lock, even though the POSIX
 * layer resolves its device through the global devoptab_list that libnx tears down when any part of
 * the process starts exiting. It is not the theoretically safer choice, it is the one that works:
 * the game's own log (sdmc:/switch/jak1/data/log/jak1.*.log) is written through exactly this path
 * from every thread for an entire run, while the direct fsFs* calls were the ones being refused
 * (KernelError_InvalidMemoryState, 0xD401) on files that plainly exist.
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

// See runtime.cpp / CodeTester.h: libnx typedefs u128 as __uint128_t while this project has a
// struct u128, so the names are swapped for the duration of the include.
#define u128 nx_u128
#define Semaphore nx_Semaphore
#include <switch.h>
#undef u128
#undef Semaphore

/*!
 * One lock for the whole FS service, shared by every file operation on this port.
 *
 * libnx's fsp-srv session is shared by all callers, and `Service` carries no internal lock (see
 * services/sm.h -- it is a bare session handle). With two threads having requests in flight on it
 * at once, the kernel rejects one set of IPC buffers with
 * MAKERESULT(Module_Kernel, KernelError_InvalidMemoryState) = 0xD401. That is exactly the code the
 * card reported:
 *
 *   File .../shaders/splash.frag cannot be opened (open rc=0xd401, exists=yes)
 *
 * "exists=yes" is the tell: fsFsGetEntryType() on that same path succeeded microseconds earlier, so
 * the file is present and it is the *request* that failed. It is intermittent and it moves between
 * runs and builds, which is the signature of a race rather than of a missing file.
 *
 * Recursive, because the locked helpers call each other -- a read takes the lock, then calls the
 * open helper which takes it again, and failure paths log through this same header.
 */
inline std::recursive_mutex& switch_fs_lock() {
  static std::recursive_mutex mtx;
  return mtx;
}

/*!
 * RAII guard for switch_fs_lock(), for call sites that touch sdmc: but are not part of the
 * fs-service layer themselves -- above all newlib's own FILE* path (fopen/fwrite/fflush/remove),
 * which resolves through fsdev to the same fsp-srv session the direct fsFs* calls use.
 *
 * Lock order, always: this lock first, then anything else (the game logger's mutex included).
 * switch_log_file() runs inside this lock and reaches lg::, and the logger takes this lock before
 * its own, so every path agrees. Taking the logger mutex first and waiting for this one would
 * deadlock the loader against whichever thread is logging.
 */
struct SwitchFsLock {
  SwitchFsLock() { switch_fs_lock().lock(); }
  ~SwitchFsLock() { switch_fs_lock().unlock(); }
  SwitchFsLock(const SwitchFsLock&) = delete;
  SwitchFsLock& operator=(const SwitchFsLock&) = delete;
};

inline std::atomic<bool>& switch_boot_log_active() {
  static std::atomic<bool> active{true};
  return active;
}

/*!
 * Latched the instant a fatal path is entered -- an assert (lg::die) or an uncaught exception
 * reaching std::terminate -- *before* abort() runs.
 *
 * abort() -> _exit -> __libnx_exit -> __appExit closes libnx's service sessions and unmounts its
 * devices, while every other thread is still running. Any of them that then touches a FILE*
 * dereferences devoptab_list[3] == NULL -- a Data Abort at offset 0x20 inside _write_r, which is
 * what the console has been reporting instead of the failure that started the exit. Once this is
 * set, every sink in this header and the game logger stop doing file I/O: the message that
 * explains the crash has already been written, and nothing written afterwards can replace it with
 * an unrelated fault.
 */
inline std::atomic<bool>& switch_process_dying() {
  static std::atomic<bool> dying{false};
  return dying;
}

// Monotonic counter of "how far did boot get". Every trace site bumps it, and the
// watchdog thread prints it, so a run that dies or wedges still tells us the last
// point that was actually reached -- including from threads that never got to log.
inline std::atomic<unsigned int>& switch_boot_stage() {
  static std::atomic<unsigned int> stage{0};
  return stage;
}

// Ungated sink behind both log entry points below. Same file, same fd, same mutex; the only
// difference is that switch_boot_log_event() does not care about the boot-only gate.
inline void switch_boot_log_write(const char* msg) {
  if (switch_process_dying().load(std::memory_order_relaxed)) {
    // See switch_process_dying(): file I/O from any thread is a fault once libnx has started
    // tearing the process down, and the messages that matter were written before the latch.
    return;
  }
  // The single FS-service lock doubles as this sink's buffer lock: every write path already takes
  // it, so one recursive lock covers both and leaves no lock-ordering question between them.
  std::lock_guard<std::recursive_mutex> lock(switch_fs_lock());

  // stdio, not the fs service. This file has arrived as 0 bytes on every single run, and the reason
  // turned out not to be the offset accounting the comments below describe: the open itself is
  // refused. What does work on this card is newlib's path -- the game's own log
  // (sdmc:/switch/jak1/data/log/jak1.*.log) is written through fopen/fwrite from the EE, IOP,
  // DECI and loader threads all run long, and at the very moments the direct fsFsOpenFile() calls
  // were returning KernelError_InvalidMemoryState (0xD401) on files that plainly exist. So the
  // diagnostic channel uses the mechanism with a proven record, and the lock above makes
  // open/write/close from any thread safe (its absence is what raced fsdev's internals originally).
  {
    static bool truncated = false;
    FILE* fp = fopen("sdmc:/gk_boot_log.txt", truncated ? "a" : "w");
    if (fp) {
      truncated = true;
      if (msg) {
        fwrite(msg, 1, strlen(msg), fp);
      }
      fflush(fp);
      fclose(fp);
    }
  }

  // There used to be a second sink here that talked to the FS service (fsFsOpenFile/fsFileWrite)
  // and recreated the file on its first write. It is gone, and the reason it took a whole
  // debugging cycle with it is worth writing down: it *deleted and recreated* sdmc:/gk_boot_log.txt
  // whenever its own open had not succeeded yet, and it ran *after* the stdio sink above. So when
  // its open kept failing -- which is exactly what it does on this card -- every single message
  // truncated the file the stdio sink had just written to, and the card ended up with a 0-byte
  // log while the stdio channel was working perfectly. That is why build23 was launched blind:
  // the run's own trace was on the card and then removed, one message at a time.
  //
  // One channel, the one with the proven record, and nothing that can truncate what it wrote.
}

inline void switch_boot_log(const char* msg) {
  if (!switch_boot_log_active().load(std::memory_order_relaxed)) {
    return;
  }
  switch_boot_log_write(msg);
}

// Lifecycle logging that must survive switch_boot_log_finish() -- runtime restarts happen long
// after boot tracing is switched off, and those are exactly the events worth keeping. The
// per-site boot tracing stays gated (several of its call sites live in the renderer, which must
// not do guest file I/O every frame); this is only for a handful of one-shot events.
inline void switch_boot_log_event(const char* msg) {
  switch_boot_log_write(msg);
}

/*!
 * Report a fatal, process-ending condition that did not go through lg::die, then latch
 * switch_process_dying(). Write first, latch second: the write is the thing that has to survive.
 */
inline void switch_fatal_report(const char* msg) {
  switch_boot_log_event(msg);
  switch_process_dying().store(true, std::memory_order_relaxed);
}

// Trace site that also advances the boot-stage counter; each call is one numbered step of boot.
inline void switch_boot_mark(const char* msg) {
  switch_boot_stage().fetch_add(1, std::memory_order_relaxed);
  switch_boot_log(msg);
}

inline void switch_boot_log_reset() {
  switch_boot_log_active().store(true, std::memory_order_relaxed);
  switch_boot_stage().store(0, std::memory_order_relaxed);
  // The stdio sink truncates on the first write of each run via a function-local flag; that flag is
  // deliberately left alone here, because it lives inside switch_boot_log_write. What the reset
  // does do is make sure the next write really happens: it clears the fs-service sink's idea of how
  // much is on the card so the whole buffer is written again.
  switch_boot_log_write("[boot_log] ---- run start ----\n");
}

inline void switch_boot_log_finish() {
  switch_boot_log("[boot_log] boot complete, tracing disabled\n");
  switch_boot_log_active().store(false, std::memory_order_relaxed);
}
