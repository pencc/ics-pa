#include <unistd.h>
#include <stdio.h>
#include <sys/time.h>

int main() {
  struct timeval start, end;
  gettimeofday(&start, NULL);
  while (1) {
    gettimeofday(&end, NULL);
    if (end.tv_sec - start.tv_sec >= 1) {
      printf("Hello world!\n");
      start = end;
    }
  }
  return 0;
}
