#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

static int evtdev = -1;
static int fbdev = -1;
static int screen_w = 0, screen_h = 0;

struct timeval NDL_startTime;

uint32_t NDL_GetTicks() {
  struct timeval now;
  gettimeofday(&now, NULL);
  return (now.tv_sec * 1000000 + now.tv_usec) - (NDL_startTime.tv_sec * 1000000 + NDL_startTime.tv_usec);
}

int NDL_PollEvent(char *buf, int len) {
  int fd;
  fd = open("/dev/events", O_RDONLY);
  return read(fd, buf, len);
}

// 打开一张(*w) X (*h)的画布
// 如果*w和*h均为0, 则将系统全屏幕作为画布, 并将*w和*h分别设为系统屏幕的大小
void NDL_OpenCanvas(int *w, int *h) {
  int dispinfo_fd;
  int dp_w, dp_h;
  char buf[64];
  dispinfo_fd = open("/proc/dispinfo", O_RDONLY);
  read(dispinfo_fd, buf, sizeof(buf));
  sscanf(buf, "%d %d", &dp_w, &dp_h);
  if(*w == 0 || *w > dp_w)
    *w = dp_w;
  if(*h == 0 || *h > dp_h)
    *h = dp_h;
  printf("screen size  w:%d; h:%d;\n", dp_w, dp_h);

  if (getenv("NWM_APP")) {
    int fbctl = 4;
    fbdev = 5;
    screen_w = *w; screen_h = *h;
    memset(buf, 0, sizeof(buf));
    int len = sprintf(buf, "%d %d", screen_w, screen_h);
    // let NWM resize the window and create the frame buffer
    write(fbctl, buf, len);
    while (1) {
      // 3 = evtdev
      int nread = read(3, buf, sizeof(buf) - 1);
      if (nread <= 0) continue;
      buf[nread] = '\0';
      if (strcmp(buf, "mmap ok") == 0) break;
    }
    close(fbctl);
  }
}

// 向画布`(x, y)`坐标处绘制`w*h`的矩形图像, 并将该绘制区域同步到屏幕上
// 图像像素按行优先方式存储在`pixels`中, 每个像素用32位整数以`00RRGGBB`的方式描述颜色
void NDL_DrawRect(uint32_t *pixels, int x, int y, int w, int h) {
}

void NDL_OpenAudio(int freq, int channels, int samples) {
}

void NDL_CloseAudio() {
}

int NDL_PlayAudio(void *buf, int len) {
  return 0;
}

int NDL_QueryAudio() {
  return 0;
}

int NDL_Init(uint32_t flags) {
  gettimeofday(&NDL_startTime, NULL);

  if (getenv("NWM_APP")) {
    evtdev = 3;
  }
  return 0;
}

void NDL_Quit() {
}
