#include <proc.h>
#include <elf.h>

#ifdef __LP64__
# define Elf_Ehdr Elf64_Ehdr
# define Elf_Phdr Elf64_Phdr
#else
# define Elf_Ehdr Elf32_Ehdr
# define Elf_Phdr Elf32_Phdr
#endif

static uintptr_t loader(PCB *pcb, const char *filename) {
  int i;
  Elf32_Ehdr ehdr;
  ramdisk_read(&ehdr, 0, sizeof(ehdr));
  Elf32_Phdr phdr[ehdr.e_phnum];
  for(i = 0; i < ehdr.e_phnum; i++) {
    ramdisk_read(phdr + i, ehdr.e_phoff + (i * sizeof(Elf32_Phdr)), sizeof(Elf32_Phdr));
    if(phdr[i].p_memsz > phdr[i].p_filesz) {
      memset((void*)(phdr[i].p_vaddr + phdr[i].p_filesz), 0, phdr[i].p_memsz - phdr[i].p_filesz);
    }
  }

  // navy-app x86 link start at 0x03000000 (ics-pa/navy-apps/scripts/x86.mk)
  return get_ramdisk_addr(ehdr.e_entry - 0x03000000);
}

void naive_uload(PCB *pcb, const char *filename) {
  uintptr_t entry = loader(pcb, filename);
  Log("Jump to entry = %p", entry);
  ((void(*)())entry) ();
}

