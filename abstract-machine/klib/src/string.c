#include <klib.h>
#include <klib-macros.h>
#include <stdint.h>

#if !defined(__ISA_NATIVE__) || defined(__NATIVE_USE_KLIB__)


size_t strlen(const char *s) {
  const char *p = s;
  while (*p) p++;
  return (size_t)(p - s);
}

char *strcpy(char *dst, const char *src) {
  char *ret = dst;
  while ((*dst++ = *src++) != '\0') { /* copy including '\0' */ }
  return ret;
}

char *strncpy(char *dst, const char *src, size_t n) {
  char *ret = dst;
  size_t i = 0;
  for (; i < n && src[i] != '\0'; i++) {
    dst[i] = src[i];
  }
  for (; i < n; i++) {
    dst[i] = '\0';
  }
  return ret;
}

char *strcat(char *dst, const char *src) {
  char *ret = dst;
  while (*dst) dst++;            /* move to end of dst */
  while ((*dst++ = *src++) != '\0') { /* append including '\0' */ }
  return ret;
}

int strcmp(const char *s1, const char *s2) {
  const unsigned char *a = (const unsigned char *)s1;
  const unsigned char *b = (const unsigned char *)s2;
  while (*a && (*a == *b)) {
    a++; b++;
  }
  return (int)(*a - *b);
}

int strncmp(const char *s1, const char *s2, size_t n) {
  if (n == 0) return 0;
  const unsigned char *a = (const unsigned char *)s1;
  const unsigned char *b = (const unsigned char *)s2;
  size_t i = 0;
  for (; i < n; i++) {
    if (a[i] != b[i] || a[i] == '\0' || b[i] == '\0') {
      return (int)(a[i] - b[i]);
    }
  }
  return 0;
}

void *memset(void *s, int c, size_t n) {
  unsigned char *p = (unsigned char *)s;
  unsigned char cc = (unsigned char)c;
  for (size_t i = 0; i < n; i++) p[i] = cc;
  return s;
}

void *memmove(void *dst, const void *src, size_t n) {
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  if (d == s || n == 0) return dst;
  if (d < s) {
    /* non-overlapping or dst before src: copy forward */
    for (size_t i = 0; i < n; i++) d[i] = s[i];
  } else {
    /* dst > src and may overlap: copy backward */
    for (size_t i = n; i > 0; i--) d[i-1] = s[i-1];
  }
  return dst;
}

void *memcpy(void *out, const void *in, size_t n) {
  /* memcpy has undefined behavior on overlap; implement simple forward copy */
  unsigned char *d = (unsigned char *)out;
  const unsigned char *s = (const unsigned char *)in;
  for (size_t i = 0; i < n; i++) d[i] = s[i];
  return out;
}

int memcmp(const void *s1, const void *s2, size_t n) {
  const unsigned char *a = (const unsigned char *)s1;
  const unsigned char *b = (const unsigned char *)s2;
  for (size_t i = 0; i < n; i++) {
    if (a[i] != b[i]) return (int)(a[i] - b[i]);
  }
  return 0;
}


#endif
