#include <fs.h>

extern size_t ramdisk_read(void *buf, size_t offset, size_t len);
extern size_t ramdisk_write(const void *buf, size_t offset, size_t len);

extern size_t serial_write(const void *buf, size_t offset, size_t len);

extern size_t events_read(void *buf, size_t offset, size_t len);

typedef size_t (*ReadFn) (void *buf, size_t offset, size_t len);
typedef size_t (*WriteFn) (const void *buf, size_t offset, size_t len);

typedef struct {
  char *name;
  size_t size;
  size_t disk_offset;
  ReadFn read;
  WriteFn write;
  size_t open_offset;
} Finfo;

enum {FD_STDIN, FD_STDOUT, FD_STDERR, FD_FB, FD_EVENTS};

size_t invalid_read(void *buf, size_t offset, size_t len) {
  panic("should not reach here");
  return 0;
}

size_t invalid_write(const void *buf, size_t offset, size_t len) {
  panic("should not reach here");
  return 0;
}

/* This is the information about all files in disk. */
static Finfo file_table[] __attribute__((used)) = {
  [FD_STDIN]  = {"stdin", 0, 0, invalid_read, invalid_write},
  [FD_STDOUT] = {"stdout", 0, 0, invalid_read, serial_write},
  [FD_STDERR] = {"stderr", 0, 0, invalid_read, serial_write},
  [FD_EVENTS] = {"/dev/events", 0, 0, events_read, invalid_write},
#include "files.h"
};

void fs_getpath_by_fd(void* path, int fd)
{
  if(fd < 0 || fd > sizeof(file_table) - 1)
    return;

  strncpy(path, file_table[fd].name, strlen(file_table[fd].name));
}

// ignore flags & mode
int fs_open(const char *pathname, int flags, int mode)
{
  int i;
  for(i = 0; i < sizeof(file_table); i++) {
    if(!strcmp(file_table[i].name, pathname))
      return i;
  }

  panic("fs_open: not found file:%s in ramdisk\n", pathname);
}

size_t fs_read(int fd, void *buf, size_t len)
{
  int done_len;

  if(fd < 0 || fd > sizeof(file_table) - 1)
    panic("fs_read: fd:%d, less than zero, or bigger than sizeof file_table(%d).\n", fd, sizeof(file_table));

  if(NULL != file_table[fd].read)
    done_len = file_table[fd].read(buf, file_table[fd].open_offset, len);
  else
    done_len = ramdisk_read(buf, file_table[fd].disk_offset + file_table[fd].open_offset, len);

  file_table[fd].open_offset += done_len;

  return done_len;
}

size_t fs_write(int fd, const void *buf, size_t len)
{
  int done_len;

  if(fd < 0 || fd > sizeof(file_table) - 1)
    panic("fs_write: fd:%d, less than zero, or bigger than sizeof file_table(%d).\n", fd, sizeof(file_table));

  if(NULL != file_table[fd].write)
    done_len = file_table[fd].write(buf, file_table[fd].open_offset, len);
  else
    done_len = ramdisk_write(buf, file_table[fd].disk_offset + file_table[fd].open_offset, len);

  file_table[fd].open_offset += done_len;

  return done_len;
}

size_t fs_lseek(int fd, size_t offset, int whence)
{
  if(fd < 0 || fd > sizeof(file_table) - 1)
    panic("fs_lseek: fd:%d, less than zero, or bigger than sizeof file_table(%d).\n", fd, sizeof(file_table));

  if(whence < SEEK_SET || whence > SEEK_END)
    panic("fs_lseek: SEEK:%d not permitted.\n", whence);

  switch(whence) {
    case SEEK_SET:
      offset = offset + 0;
      break;
    case SEEK_CUR:
      offset = offset + file_table[fd].open_offset;
      break;
    case SEEK_END:
      offset = offset + file_table[fd].size;
      break;
  }

  if(offset < 0 || offset > file_table[fd].size)
    return -1;

  file_table[fd].open_offset = offset;

  return file_table[fd].open_offset;
}

int fs_close(int fd)
{
  return 0;
}

void init_fs() {
  // TODO: initialize the size of /dev/fb
}
