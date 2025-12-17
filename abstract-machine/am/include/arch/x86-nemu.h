#ifndef ARCH_H__
#define ARCH_H__

struct Context {
  void *cr3;
  uintptr_t esi, ebx, eax, eip, edx, eflags, ecx, cs, esp, edi, ebp;
  int irq;
};

/**
 * #if defined(__ISA_X86__)
 * # define ARGS_ARRAY ("int $0x80", "eax", "ebx", "ecx", "edx", "eax")
 */

#define GPR1 eax
#define GPR2 ebx
#define GPR3 ecx
#define GPR4 edx
#define GPRx eax

#endif
