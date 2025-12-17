#include <common.h>

extern uint8_t ramdisk_start;
extern uint8_t ramdisk_end;
#define RAMDISK_SIZE ((&ramdisk_end) - (&ramdisk_start))

/* The kernel is monolithic, therefore we do not need to
 * translate the address `buf' from the user process to
 * a physical one, which is necessary for a microkernel.
 */

/* read `len' bytes starting from `offset' of ramdisk into `buf' */
size_t ramdisk_read(void *buf, size_t offset, size_t len) {
  assert(offset + len <= RAMDISK_SIZE);
  memcpy(buf, &ramdisk_start + offset, len);
  return len;
}

/* write `len' bytes starting from `buf' into the `offset' of ramdisk */
size_t ramdisk_write(const void *buf, size_t offset, size_t len) {
  assert(offset + len <= RAMDISK_SIZE);
  memcpy(&ramdisk_start + offset, buf, len);
  return len;
}

void print_ramdisk() {
    uint8_t *start = &ramdisk_start;
    uint8_t *end   = &ramdisk_end;
    size_t size = end - start;

    for (size_t i = 0; i < size; i += 16) {
        printf("%p: ", start + i);

        for (size_t j = 0; j < 16 && i + j < size; j++) {
            printf("%x ", start[i + j]);
        }
        printf("\n");
    }
}

void init_ramdisk() {
  Log("ramdisk s=%p, e=%p, sz=%d bytes\n",
      &ramdisk_start, &ramdisk_end, RAMDISK_SIZE);
  //print_ramdisk();
}

size_t get_ramdisk_addr(size_t addr) {
  return addr;
}

size_t get_ramdisk_start_addr() {
  return (size_t)&ramdisk_start;
}

size_t get_ramdisk_size() {
  return RAMDISK_SIZE;
}
