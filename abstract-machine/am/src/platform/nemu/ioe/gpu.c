#include <am.h>
#include <nemu.h>

#define SYNC_ADDR (VGACTL_ADDR + 4)

int screen_w, screen_h;

void __am_gpu_init() {
  int screen_w_h;
  screen_w_h = inl(VGACTL_ADDR);
  screen_w = (screen_w_h >> 16) & 0xFFFF;
  screen_h = screen_w_h & 0xFFFF;
}

void __am_gpu_config(AM_GPU_CONFIG_T *cfg) {
  uint32_t screen_w_h;
  *cfg = (AM_GPU_CONFIG_T) {
    .present = true, .has_accel = false,
    .width = 0, .height = 0,
    .vmemsz = 0
  };
  screen_w_h = inl(VGACTL_ADDR);
  cfg->width = (screen_w_h >> 16) & 0xFFFF;
  cfg->height = screen_w_h & 0xFFFF;
  cfg->vmemsz = cfg->width * cfg->height * sizeof(uint32_t);
}

void __am_gpu_fbdraw(AM_GPU_FBDRAW_T *ctl) {
  uint32_t  start_x, start_y, w, h;
  uint32_t  *fb = (uint32_t *)(uintptr_t)FB_ADDR;
  uint32_t  *pix = ctl->pixels;
  uint32_t  i, j;

  start_x = ctl->x;
  start_y = ctl->y;
  w = ctl->w;
  h = ctl->h;

  // move draw pointer to start point at [start_x, start_y]
  fb += (start_y * screen_w + start_x);
  for(j = 0; j < h; j++) {
    for(i = 0; i < w; i++) {
      *(fb + i) = *(pix + j * w + i);
    }
    // move fb to next row at start_x
    fb += screen_w;
  }

  if (ctl->sync) {
    outl(SYNC_ADDR, 1);
  }
}

void __am_gpu_status(AM_GPU_STATUS_T *status) {
  status->ready = true;
}
