#include <common.h>
#include "syscall.h"
#include "fs.h"

const char *SYSCALL_INDEX[] = 
{
  "SYS_exit",
  "SYS_yield",
  "SYS_open",
  "SYS_read",
  "SYS_write",
  "SYS_kill",
  "SYS_getpid",
  "SYS_close",
  "SYS_lseek",
  "SYS_brk",
  "SYS_fstat",
  "SYS_time",
  "SYS_signal",
  "SYS_execve",
  "SYS_fork",
  "SYS_link",
  "SYS_unlink",
  "SYS_wait",
  "SYS_times",
  "SYS_gettimeofday"
};

void do_syscall(Context *c) {
  uintptr_t a[4];
  a[0] = c->GPR1;
  a[1] = c->GPR2;
  a[2] = c->GPR3;
  a[3] = c->GPR4;

#if defined(__STRACE__)
  printf("\nSTRACE-CALL [#%s]( %d, %d, %x )\n", SYSCALL_INDEX[a[0]], a[1], a[2], a[3]);
#endif
  switch (a[0]) {
    case SYS_yield:
      c->GPRx = 0;
      break;
    case SYS_exit:
      c->GPRx = 0;
      halt(0);
      break;
    case SYS_open: {
      long pathname = a[1];
      int flags = a[2];
      int mode = a[3];
      c->GPRx = fs_open((const char*)pathname, flags, mode);
      break;
    }
    case SYS_read: {
      int fd = a[1];
      long buf = a[2];
      int len = a[3];
      c->GPRx = fs_read(fd, (void *)buf, len);
      break;
    }
    case SYS_write: {
      int fd = a[1];
      long addr = a[2];
      int count = a[3];
      c->GPRx = fs_write(fd, (void *)addr, count);
      break;
    }
    case SYS_close: {
      int fd = a[1];
      c->GPRx = fs_close(fd);
      break;
    }
    case SYS_lseek: {
      int fd = a[1];
      size_t offset = a[2];
      int whence = a[3];
      c->GPRx = fs_lseek(fd, offset, whence);
      break;
    }
    case SYS_brk:
      // TODO: 目前Nanos-lite还是一个单任务操作系统, 空闲的内存都可以让用户程序自由使用, 
      // 因此我们只需要让SYS_brk系统调用总是返回0即可, 表示堆区大小的调整总是成功.
      c->GPRx = 0;
      break;
    default: panic("Unhandled syscall ID = %d", a[0]);
  }

#if defined(__STRACE__)
  printf("\nSTRACE-END  [#%s] ret=%d\n", SYSCALL_INDEX[a[0]], c->GPRx);
#endif
}
