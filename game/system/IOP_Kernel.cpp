#include "IOP_Kernel.h"

#include <cstring>

#include "common/global_profiler/GlobalProfiler.h"
#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/FileUtil.h"

#include "game/sce/iop.h"

#if defined(__SWITCH__)
#include "game/switch/boot_log.h"
#define IOP_BOOT_LOG(msg) switch_boot_mark(msg)
// Called from the C trampoline in libco's aarch64.c; declared there as plain C.
extern "C" void co_boot_trace(const char* msg) {
  switch_boot_log(msg);
}
#else
#define IOP_BOOT_LOG(msg) ((void)0)
#endif

using namespace std::chrono;

/*!
 * Report an exception that escaped an RPC handler or an IOP thread entry.
 *
 * Both call sites run on libco coroutine threads, and there is no handler above them: anything
 * thrown out of them reaches std::terminate, which aborts the whole console and (through
 * _exit -> __libnx_exit -> __appExit) tears down libnx's devices while every other thread is still
 * running. A file error from the guest therefore used to cost the process -- and, because the
 * teardown can take out an unrelated thread's file write, the crash report named the wrong code.
 * The message goes to both channels that survive (see boot_log.h) so the real reason is on the
 * card even if the run dies later for an unrelated reason.
 */
static void iop_report_exception(const char* phase, const char* what) {
#if defined(__SWITCH__)
  const std::string msg = fmt::format("[iop] {} threw: {}\n", phase, what);
  switch_boot_log_event(msg.c_str());
  lg::error("[iop] {} threw: {}", phase, what);
#else
  lg::error("[iop] {} threw: {}", phase, what);
#endif
}

/*
** wrap thread entry points to ensure they don't return into libco
*/
static void (*thread_entry)() = nullptr;
static cothread_t wrap_return;
void IopThread::functionWrapper() {
  IOP_BOOT_LOG("[iop_kernel] functionWrapper entered\n");
  void (*f)() = thread_entry;
  co_switch(wrap_return);
  IOP_BOOT_LOG("[iop_kernel] functionWrapper resumed, calling real entry\n");
  if (f != nullptr) {
    // Last resort: a thread entry that throws must not take the process with it. The thread stops
    // making progress (the loop below parks it), which is reported loudly instead of silently
    // killing the console.
    try {
      f();
    } catch (const std::exception& e) {
      iop_report_exception("an IOP thread entry", e.what());
    } catch (...) {
      iop_report_exception("an IOP thread entry", "unknown exception");
    }
  }
  // libco threads must not return
  while (true) {
    iop::ExitThread();
  }
}

IOP_Kernel::IOP_Kernel() {
  IOP_BOOT_LOG("[iop_kernel] ctor enter\n");
  // this ugly hack
  threads.reserve(16);
  IOP_BOOT_LOG("[iop_kernel] threads reserved\n");
  CreateThread("null-thread", nullptr, 0);
  IOP_BOOT_LOG("[iop_kernel] null-thread created\n");
  CreateMbx();
  IOP_BOOT_LOG("[iop_kernel] mbx created\n");
  CreateSema(0, 0, 0, 0);
  IOP_BOOT_LOG("[iop_kernel] sema created\n");
  kernel_thread = co_active();
  IOP_BOOT_LOG("[iop_kernel] kernel_thread set, ctor done\n");
  m_start_time = time_point_cast<microseconds>(steady_clock::now());
}

/*
** -----------------------------------------------------------------------------
** Functions callable by threads
** -----------------------------------------------------------------------------
*/

u32 IOP_Kernel::GetSystemTimeLow() {
  auto delta_time = time_point_cast<microseconds>(steady_clock::now()) - m_start_time;
  return static_cast<u32>(static_cast<u64>(delta_time.count() * 36.864));
}

/*!
 * Create a new thread.  Will not run the thread.
 */
s32 IOP_Kernel::CreateThread(std::string name, void (*func)(), u32 priority) {
  u32 ID = (u32)_nextThID++;
  ASSERT(ID == threads.size());

  // add entry
  IOP_BOOT_LOG("[iop_kernel] CreateThread about to emplace (runs co_create)\n");
  threads.emplace_back(name, func, ID, priority);
  IOP_BOOT_LOG("[iop_kernel] CreateThread emplaced entry\n");

  // enter the function wrapper so it can put the actual thread enry on its stack
  // to call it when the thread is eventually started
  thread_entry = func;
  wrap_return = co_active();
  IOP_BOOT_LOG("[iop_kernel] CreateThread about to co_switch in\n");
  co_switch(threads.at(ID).thread);
  IOP_BOOT_LOG("[iop_kernel] CreateThread returned from co_switch\n");

  return ID;
}

/*!
 * Start a thread. Marking it to run on each dispatch of the IOP kernel.
 */
void IOP_Kernel::StartThread(s32 id) {
  threads.at(id).waitType = IopThread::Wait::None;
  threads.at(id).state = IopThread::State::Ready;
}

s32 IOP_Kernel::ExitThread() {
  ASSERT(_currentThread);
  _currentThread->state = IopThread::State::Dormant;

  return 0;
}

/*!
 * Put a thread in Wait state for desired amount of usecs.
 */
void IOP_Kernel::DelayThread(u32 usec) {
  ASSERT(_currentThread);

  _currentThread->state = IopThread::State::Wait;
  _currentThread->waitType = IopThread::Wait::Delay;
  _currentThread->resumeTime =
      time_point_cast<microseconds>(steady_clock::now()) + microseconds(usec);
  leaveThread();
}

/*!
 * Sleep a thread.  Must be explicitly woken up.
 */
void IOP_Kernel::SleepThread() {
  ASSERT(_currentThread);

  if (_currentThread->wakeupCount > 0) {
    _currentThread->wakeupCount--;
    return;
  }

  _currentThread->state = IopThread::State::Wait;
  _currentThread->waitType = IopThread::Wait::Sleep;

  leaveThread();
}

void IOP_Kernel::YieldThread() {
  ASSERT(_currentThread);
  _currentThread->waitType = IopThread::Wait::None;
  _currentThread->state = IopThread::State::Ready;
  leaveThread();
}

/*!
 * Wake up a thread. Doesn't run it immediately though.
 */
void IOP_Kernel::WakeupThread(s32 id) {
  ASSERT(id > 0);

  auto& thread = threads.at(id);
  if (thread.state != IopThread::State::Wait || thread.waitType != IopThread::Wait::Sleep) {
    thread.wakeupCount++;
    return;
  }

  threads.at(id).waitType = IopThread::Wait::None;
  threads.at(id).state = IopThread::State::Ready;
}

void IOP_Kernel::iWakeupThread(s32 id) {
  ASSERT(id > 0);
  std::scoped_lock lock(wakeup_mtx);
  wakeup_queue.push(id);
}

s32 IOP_Kernel::WaitSema(s32 id) {
  auto& sema = semas.at(id);
  if (sema.count > 0) {
    sema.count--;
    return KE_OK;
  }

  sema.wait_list.push_back(_currentThread);
  _currentThread->state = IopThread::State::Wait;
  _currentThread->waitType = IopThread::Wait::Semaphore;
  leaveThread();

  return KE_OK;
}

s32 IOP_Kernel::ClearEventFlag(s32 id, u32 pattern) {
  auto& ef = event_flags.at(id);
  // yes, this seems backward, but the manual says this is how it works.
  ef.value &= pattern;
  return 0;
}

namespace {
bool event_flag_check(u32 pattern, u32 check_pattern, u32 mode) {
  if (mode & 1) {
    // or
    return (pattern & check_pattern);
  } else {
    // and
    return (pattern & check_pattern) == check_pattern;
  }
}
}  // namespace

s32 IOP_Kernel::WaitEventFlag(s32 flag, u32 pattern, u32 mode) {
  auto& ef = event_flags.at(flag);
  // check to see if we already match
  if (event_flag_check(ef.value, pattern, mode)) {
    if (mode & 0x10) {
      ef.value = 0;
    }
    return KE_OK;
  } else {
    if (!ef.multiple_waiters_allowed && !ef.wait_list.empty()) {
      lg::die("Multiple thread trying to wait on an event flag, but this option was not enabled.");
    }

    auto& wait_entry = ef.wait_list.emplace_back();
    wait_entry.pattern = pattern;
    wait_entry.mode = mode;
    wait_entry.thread = _currentThread;

    _currentThread->state = IopThread::State::Wait;
    _currentThread->waitType = IopThread::Wait::EventFlag;
    leaveThread();

    return KE_OK;
  }
}

s32 IOP_Kernel::SetEventFlag(s32 flag, u32 pattern) {
  auto& ef = event_flags.at(flag);
  ef.value |= pattern;

  for (auto it = ef.wait_list.begin(); it != ef.wait_list.end();) {
    if (event_flag_check(ef.value, it->pattern, it->mode)) {
      if (it->mode & 0x10) {
        ef.value = 0;
      }
      it->thread->waitType = IopThread::Wait::None;
      it->thread->state = IopThread::State::Ready;
      it = ef.wait_list.erase(it);
    } else {
      ++it;
    }
  }

  return KE_OK;
}

s32 IOP_Kernel::ReceiveMbx(void** msg, s32 id) {
  auto& box = mbxs.at(id);
  if (!box.messages.empty()) {
    auto ret = PollMbx(msg, id);
    ASSERT(ret == KE_OK);
    return KE_OK;
  }

  ASSERT(!box.wait_thread);  // don't know how to deal with this, hopefully doesn't come up.

  box.wait_thread = _currentThread;
  _currentThread->state = IopThread::State::Wait;
  _currentThread->waitType = IopThread::Wait::Messagebox;
  leaveThread();

  auto ret = PollMbx(msg, id);
  ASSERT(ret == KE_OK);
  return KE_OK;
}

s32 IOP_Kernel::SendMbx(s32 mbx, void* value) {
  ASSERT(mbx < (s32)mbxs.size());
  auto& box = mbxs[mbx];
  box.messages.push(value);
  auto* to_run = box.wait_thread;

  if (to_run) {
    box.wait_thread = nullptr;
    to_run->waitType = IopThread::Wait::None;
    to_run->state = IopThread::State::Ready;
  }
  return 0;
}

s32 IOP_Kernel::SignalSema(s32 id) {
  auto& sema = semas.at(id);

  if (sema.count >= sema.maxCount) {
    return KE_SEMA_OVF;
  }

  if (sema.wait_list.empty()) {
    sema.count++;
    return KE_OK;
  }

  IopThread* to_run = nullptr;

  if (sema.attr == Semaphore::attribute::fifo) {
    to_run = sema.wait_list.front();
    sema.wait_list.pop_front();
  } else {
    auto it =
        std::max_element(sema.wait_list.begin(), sema.wait_list.end(),
                         [](IopThread*& a, IopThread*& b) { return a->priority < b->priority; });
    to_run = *it;
    sema.wait_list.erase(it);
  }

  to_run->waitType = IopThread::Wait::None;
  to_run->state = IopThread::State::Ready;
  return KE_OK;
}

s32 IOP_Kernel::PollSema(s32 id) {
  auto& sema = semas.at(id);
  if (sema.count > 0) {
    sema.count--;
    ASSERT(sema.count >= 0);
    return KE_OK;
  }

  return KE_SEMA_ZERO;
}

/*!
 * Return to kernel from a thread, not to be called from the kernel thread.
 */
void IOP_Kernel::leaveThread() {
  IopThread* oldThread = _currentThread;
  co_switch(kernel_thread);

  // check kernel resumed us correctly
  ASSERT(_currentThread == oldThread);
}

/*
** -----------------------------------------------------------------------------
** Kernel functions.
** -----------------------------------------------------------------------------
*/

/*!
 * Run a thread (call from kernel)
 */
void IOP_Kernel::runThread(IopThread* thread) {
  ASSERT(_currentThread == nullptr);  // should run in the kernel thread
  _currentThread = thread;
  thread->state = IopThread::State::Run;
  co_switch(thread->thread);
  _currentThread = nullptr;
}

/*!
** Update wait states for delayed threads
*/
void IOP_Kernel::updateDelay() {
  for (auto& t : threads) {
    if (t.waitType == IopThread::Wait::Delay) {
      if (steady_clock::now() > t.resumeTime) {
        t.waitType = IopThread::Wait::None;
        t.state = IopThread::State::Ready;
      }
    }
  }
}

std::optional<time_stamp> IOP_Kernel::nextWakeup() {
  bool found_ready = false;
  time_stamp lowest = time_point_cast<microseconds>(steady_clock::now()) + microseconds(1000);

  for (auto& t : threads) {
    if (t.waitType == IopThread::Wait::Delay) {
      if (t.resumeTime < lowest) {
        lowest = t.resumeTime;
      }
    }

    if (t.state == IopThread::State::Ready) {
      found_ready = true;
    }
  }

  if (found_ready) {
    return {};
  } else {
    return lowest;
  }
}

/*!
** Get next thread to run.
** i.e. Highest prio in ready state.
*/
IopThread* IOP_Kernel::schedNext() {
  IopThread* highest_prio = nullptr;

  for (auto& t : threads) {
    if (t.state == IopThread::State::Ready) {
      if (highest_prio == nullptr) {
        highest_prio = &t;
      }

      // Lower number = higher priority
      if (t.priority < highest_prio->priority) {
        highest_prio = &t;
      }
    }
  }

  return highest_prio;
};

void IOP_Kernel::processWakeups() {
  std::scoped_lock lock(wakeup_mtx);
  while (!wakeup_queue.empty()) {
    WakeupThread(wakeup_queue.front());
    wakeup_queue.pop();
  }
}

/*!
 * Run the next IOP thread.
 */
std::optional<time_stamp> IOP_Kernel::dispatch() {
  // Update thread states
  updateDelay();
  processWakeups();

  // Run until all threads are idle
  IopThread* next = schedNext();
  if (next) {
    prof().root_event();
  }
  while (next != nullptr) {
    // Check vblank interrupt
    if (vblank_handler != nullptr && vblank_recieved) {
      vblank_handler(nullptr);
      vblank_recieved = false;
    }
    // printf("[IOP Kernel] Dispatch %s (%d)\n", next->name.c_str(), next->thID);
    auto p = scoped_prof(next->name.c_str());
    runThread(next);
    updateDelay();
    processWakeups();
    next = schedNext();
    // printf("[IOP Kernel] back to kernel!\n");
  }

  // printf("[IOP Kernel] No runnable threads\n");
  return nextWakeup();
}

void IOP_Kernel::set_rpc_queue(iop::sceSifQueueData* qd, u32 thread) {
  sif_mtx.lock();
  for (const auto& r : sif_records) {
    ASSERT(!(r.qd == qd || r.thread_to_wake == thread));
  }
  SifRecord rec;
  rec.thread_to_wake = thread;
  rec.qd = qd;
  sif_records.push_back(rec);
  sif_mtx.unlock();
}

typedef void* (*sif_rpc_handler)(unsigned int, void*, int);

bool IOP_Kernel::sif_busy(u32 id) {
  sif_mtx.lock();
  bool rv = false;
  bool found = false;
  for (auto& r : sif_records) {
    if (r.qd->serve_data->command == id) {
      rv = !r.cmd.finished;
      found = true;
      break;
    }
  }
  ASSERT(found);
  sif_mtx.unlock();
  return rv;
}

void IOP_Kernel::sif_rpc(s32 rpcChannel,
                         u32 fno,
                         bool async,
                         void* sendBuff,
                         s32 sendSize,
                         void* recvBuff,
                         s32 recvSize) {
  ASSERT(async);
  sif_mtx.lock();
  // step 1 - find entry
  SifRecord* rec = nullptr;
  for (auto& e : sif_records) {
    if (e.qd->serve_data->command == (u32)rpcChannel) {
      rec = &e;
    }
  }
  if (!rec) {
    printf("Failed to find handler for sif channel 0x%x\n", rpcChannel);
  }
  ASSERT(rec);

  // step 2 - check entry is safe to give command to
  ASSERT(rec->cmd.finished && rec->cmd.started);

  // step 3 - memcpy!
  if (rec->qd->serve_data->buff_size < sendSize) {
    lg::die(
        "Buffer overflow in EE -> IOP RPC. channel {}, fno {}, requested size {}, buffer size {}\n",
        rpcChannel, fno, sendSize, rec->qd->serve_data->buff_size);
  }
  memcpy(rec->qd->serve_data->buff, sendBuff, sendSize);

  // step 4 - setup command
  rec->cmd.buff = rec->qd->serve_data->buff;
  rec->cmd.size = sendSize;
  rec->cmd.fno = fno;
  rec->cmd.copy_back_buff = recvBuff;
  rec->cmd.copy_back_size = recvSize;
  rec->cmd.started = false;
  rec->cmd.finished = false;

  iWakeupThread(rec->thread_to_wake);

  sif_mtx.unlock();
}

void IOP_Kernel::rpc_loop(iop::sceSifQueueData* qd) {
  while (true) {
    bool got_cmd = false;
    SifRpcCommand cmd;
    sif_rpc_handler func = nullptr;

    // get command and mark it as started if we get it
    sif_mtx.lock();
    for (auto& r : sif_records) {
      if (r.qd == qd) {
        cmd = r.cmd;
        got_cmd = true;
        r.cmd.started = true;
        func = r.qd->serve_data->func;
      }
    }
    sif_mtx.unlock();

    // handle command
    if (got_cmd) {
      if (!cmd.started) {
        // cf
        ASSERT(func);
        // Nothing a handler does may kill the process. This loop is a libco coroutine thread with
        // nothing above it that catches, so an exception out of a handler used to reach
        // std::terminate (the title-screen crash: fake_iso's FS_GetLength -> ghc::filesystem
        // status() on an sdmc path, which throws filesystem_error when the devoptab stat fails
        // with anything but ENOENT). Failing is a normal outcome at this layer -- the handlers
        // already return nullptr for "not found" -- so report it and answer the guest with an
        // empty buffer instead of taking the console down.
        void* data = nullptr;
        try {
          data = func(cmd.fno, cmd.buff, cmd.size);
        } catch (const std::exception& e) {
          iop_report_exception("an RPC handler", e.what());
        } catch (...) {
          iop_report_exception("an RPC handler", "unknown exception");
        }
        if (cmd.copy_back_buff && cmd.copy_back_size) {
          // memcpy from a null answer is itself a fault; an empty buffer is the failure signal the
          // guest reads.
          if (data) {
            memcpy(cmd.copy_back_buff, data, cmd.copy_back_size);
          } else {
            memset(cmd.copy_back_buff, 0, cmd.copy_back_size);
          }
        }

        sif_mtx.lock();
        for (auto& r : sif_records) {
          if (r.qd == qd) {
            ASSERT(r.cmd.started);
            r.cmd.finished = true;
          }
        }
        sif_mtx.unlock();
      }
    }

    SleepThread();
  }
}
