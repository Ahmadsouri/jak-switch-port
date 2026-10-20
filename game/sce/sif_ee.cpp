#include "sif_ee.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/FileUtil.h"

#include "game/runtime.h"
#include "game/system/iop_thread.h"

namespace ee {

namespace {
::IOP* iop;
std::unordered_map<s32, FILE*> sce_fds;
}  // namespace

void LIBRARY_sceSif_register(::IOP* i) {
  iop = i;
}

void LIBRARY_INIT_sceSif() {
  iop = nullptr;
  for (auto& kv : sce_fds) {
    fclose(kv.second);
  }
  sce_fds.clear();
}
void sceSifInitRpc(unsigned int mode) {
  (void)mode;
}

int sceSifRebootIop(const char* imgfile) {
  (void)imgfile;
  return 1;
}

int sceSifSyncIop() {
  return 1;
}

void sceFsReset() {}

int sceSifLoadModule(const char* name, int arg_size, const char* args) {
  if (!strcmp(name, "cdrom0:\\\\DRIVERS\\\\OVERLORD.IRX;1") ||
      !strcmp(name, "host0:binee/overlord.irx") || !strcmp(name, "host0:bin/overlord.irx")) {
    const char* src = args;
    char* dst = iop->overlord_arg_data;
    int cnt;
    iop->overlord_argv[0] = nullptr;
    for (cnt = 1; src - args < arg_size; cnt++) {
      auto len = strlen(src);
      memcpy(dst, src, len + 1);
      iop->overlord_argv[cnt] = dst;
      dst += len + 1;
      src += len + 1;
    }
    iop->overlord_argc = cnt;

    for (int i = 0; i < cnt; i++) {
      if (iop->overlord_argv[i])
        printf("arg %d : %s\n", i, iop->overlord_argv[i]);
    }
    iop->set_ee_main_mem(g_ee_main_mem);
    iop->send_status(IOP_Status::IOP_OVERLORD_INIT);
    iop->wait_for_overlord_init_finish();
  }

  return 1;
}

s32 sceSifCallRpc(sceSifClientData* bd,
                  u32 fno,
                  u32 mode,
                  void* send,
                  s32 ssize,
                  void* recv,
                  s32 rsize,
                  void* end_func,
                  void* end_para) {
  ASSERT(!end_func);
  ASSERT(!end_para);
  ASSERT(mode == 1);  // async
  iop->kernel.sif_rpc(bd->rpcd.id, fno, mode, send, ssize, recv, rsize);
  iop->signal_run_iop();
  return 0;
}

s32 sceSifCheckStatRpc(sceSifRpcData* bd) {
  iop->signal_run_iop();
  return iop->kernel.sif_busy(bd->id);
}

s32 sceSifBindRpc(sceSifClientData* bd, u32 request, u32 mode) {
  ASSERT(mode == 1);  // async
  bd->rpcd.id = request;
  bd->serve = (sceSifServeData*)1;
  return 0;
}

s32 sceOpen(const char* filename, s32 flag) {
  FILE* fp = nullptr;
  auto name = file_util::get_file_path({filename});
  switch (flag) {
    case SCE_RDONLY: {
      fp = file_util::open_file(name.c_str(), "rb");
    } break;

    default: {
      // either append or truncate
      file_util::create_dir_if_needed_for_file(name);
      if (flag & SCE_TRUNC) {
        fp = file_util::open_file(name.c_str(), "w");
      } else {
        fp = file_util::open_file(name.c_str(), "a+");
      }
    } break;
  }
  if (!fp) {
    // lg::, not printf: on this port stdout is redirected to sdmc:/gk_stdout.txt, which has stayed a
    // 0-byte file on every run, so a failure reported there is a failure nobody ever reads. The
    // guest cannot see this either -- kopen stores the -1 and the caller then writes to it -- which
    // is how a failed settings write turns into the assert in sceWrite below.
    lg::error("[SCE] sceOpen(\"{}\") failed (flag {:#x})", name, (u32)flag);
    return -1;
  }

  // A counter, not sce_fds.size() + 1: closing a stream leaves the table smaller again, so the size
  // hands out an fd that is still in use and two open files then share one entry -- silently
  // writing one file's bytes into another.
  static s32 s_next_sce_fd = 1;
  const s32 fp_idx = s_next_sce_fd++;
  sce_fds[fp_idx] = fp;
  lg::debug("[SCE] sceOpen(\"{}\") -> fd {}", name, fp_idx);
  return fp_idx;
}

s32 sceMkDir(const char* filename, s32 flag) {
  return -1;
}

s32 sceClose(s32 fd) {
  if (fd < 0) {
    // todo, what should we really return?
    return 0;
  }

  auto kv = sce_fds.find(fd);
  if (kv != sce_fds.end()) {
    fclose(kv->second);
    sce_fds.erase(fd);
    return 0;
  } else {
    printf("[SCE] sceClose called on invalid fd\n");
    return 0;
  }
}

s32 sceRead(s32 fd, void* buf, s32 nbyte) {
  auto kv = sce_fds.find(fd);
  if (kv == sce_fds.end()) {
    return -1;
  } else {
    return fread(buf, 1, nbyte, kv->second);
  }
}

s32 sceWrite(s32 fd, const void* buf, s32 nbyte) {
  auto kv = sce_fds.find(fd);
  if (kv == sce_fds.end()) {
    // Deliberately not an ASSERT. It fires whenever a write follows a failed open -- exactly what
    // kopen() does when sceOpen() cannot create the file (it stores the -1 and the guest writes to
    // it anyway) -- and killing the process over a settings file the player did not ask for is the
    // wrong trade. Mirrors sceRead(), which has always reported an unknown descriptor instead.
    lg::error("[SCE] sceWrite on unknown fd {} ({} bytes dropped)", fd, nbyte);
    return -1;
  } else {
    return fwrite(buf, 1, nbyte, kv->second);
  }
}

s32 sceLseek(s32 fd, s32 offset, s32 where) {
  auto kv = sce_fds.find(fd);
  if (kv == sce_fds.end()) {
    return -1;
  } else {
    switch (where) {
      case SCE_SEEK_CUR:
        fseek(kv->second, offset, SEEK_CUR);
        return ftell(kv->second);
      case SCE_SEEK_END:
        fseek(kv->second, offset, SEEK_END);
        return ftell(kv->second);
      case SCE_SEEK_SET:
        fseek(kv->second, offset, SEEK_SET);
        return ftell(kv->second);
      default:
        ASSERT(false);
        return -1;
    }
  }
}

}  // namespace ee
