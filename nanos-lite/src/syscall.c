#include <common.h>
#include "syscall.h"

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
    case SYS_write:
      int fd = a[1];
      long addr = a[2];
      int count = a[3];
      if(1 == fd || 2 == fd) { // stdout & stderr
        for(int i = 0; i < count; i++)
          putch(*(intptr_t*)(addr + i));
        c->GPRx = count;
      } else { // TODO:

      }
      break;
    default: panic("Unhandled syscall ID = %d", a[0]);
  }

#if defined(__STRACE__)
  printf("\nSTRACE-END  [#%s] ret=%d\n", SYSCALL_INDEX[a[0]], c->GPRx);
#endif
}
