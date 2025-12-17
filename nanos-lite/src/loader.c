#include <proc.h>
#include <elf.h>

#ifdef __LP64__
# define Elf_Ehdr Elf64_Ehdr
# define Elf_Phdr Elf64_Phdr
#else
# define Elf_Ehdr Elf32_Ehdr
# define Elf_Phdr Elf32_Phdr
#endif

#if defined(__ISA_AM_NATIVE__)
# define EXPECT_TYPE EM_X86_64
#elif defined(__ISA_X86__)
# define EXPECT_TYPE EM_386
#else
# error Unsupported ISA
#endif

extern size_t ramdisk_read(void *buf, size_t offset, size_t len);
extern size_t get_ramdisk_addr(size_t addr);


static uintptr_t loader(PCB *pcb, const char *filename) {
  int i;
  Elf32_Ehdr ehdr;

  ramdisk_read(&ehdr, 0, sizeof(ehdr));
  assert(*(uint32_t *)ehdr.e_ident == 0x464c457f);
  assert(ehdr.e_machine == EXPECT_TYPE);

  Elf32_Phdr phdr[ehdr.e_phnum];
  for(i = 0; i < ehdr.e_phnum; i++) {
    ramdisk_read((void *)(phdr + i), ehdr.e_phoff + (i * sizeof(Elf32_Phdr)), sizeof(Elf32_Phdr));
    ramdisk_read((void *)phdr[i].p_vaddr, phdr[i].p_offset, phdr[i].p_filesz);
    if(phdr[i].p_memsz > phdr[i].p_filesz) {
      memset((void*)(phdr[i].p_vaddr + phdr[i].p_filesz), 0, phdr[i].p_memsz - phdr[i].p_filesz);
    }
  }

  return get_ramdisk_addr(ehdr.e_entry);
}

void naive_uload(PCB *pcb, const char *filename) {
  uintptr_t entry = loader(pcb, filename);
  Log("Jump to entry = %p", entry);
  ((void(*)())entry) ();
}

