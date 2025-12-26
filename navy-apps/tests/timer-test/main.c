#include <unistd.h>
#include <stdio.h>
#include <NDL.h>

int main() {
  uint32_t last_tick = NDL_GetTicks();
  while (1) {
    uint32_t tick = NDL_GetTicks();
    if (tick - last_tick >= 500000) {
      printf("Hello world!\n");
      last_tick = tick;
    }
  }
  return 0;
}
