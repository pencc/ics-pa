#include <am.h>
#include <klib.h>
#include <klib-macros.h>
#include <stdarg.h>

#if !defined(__ISA_NATIVE__) || defined(__NATIVE_USE_KLIB__)

static void putchstd(char **out, char ch) {
  *(*out)++ = ch;
}

static void print_num(char **out, unsigned int num, int base, int sign) {
  char buf[32];
  int i = 0;

  // 处理有符号数
  unsigned int n = num;
  if (sign && (int)num < 0) {
    putchstd(out, '-');
    n = (unsigned int)(-((int)num));
  }

  // 转换为字符串（倒序）
  if (n == 0) {
    buf[i++] = '0';
  } else {
    while (n > 0) {
      int digit = n % base;
      buf[i++] = (digit < 10) ? '0' + digit : 'a' + (digit - 10);
      n /= base;
    }
  }

  // 反向输出
  while (i--) putchstd(out, buf[i]);
}

static void putchstd_n(char **out, char ch, char *end) {
  if (*out < end) {
    **out = ch;
    (*out)++;
  }
}

static void print_num_n(char **out, unsigned int val, int base, int sign, char *end) {
  char buf[32];
  int i = 0;
  if (sign && (int)val < 0) {
    val = -(int)val;
    putchstd_n(out, '-', end);
  }
  do {
    int digit = val % base;
    buf[i++] = (digit < 10) ? '0' + digit : 'a' + (digit - 10);
    val /= base;
  } while (val);
  while (i > 0) {
    putchstd_n(out, buf[--i], end);
  }
}

int printf(const char *fmt, ...) {
  char out[128];
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(out, sizeof(out), fmt, ap);
  va_end(ap);

  for (int i = 0; i < len && out[i]; i++) {
    putch(out[i]);  // 输出到终端
  }

  return len;
}

int vsprintf(char *out, const char *fmt, va_list ap) {
  panic("Not implemented");
}

int sprintf(char *out, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);

  char *start = out;

  for (; *fmt; fmt++) {
    if (*fmt != '%') {
      putchstd(&out, *fmt);
      continue;
    }

    fmt++;  // 跳过 '%'

    switch (*fmt) {
      case 'd':
        print_num(&out, va_arg(ap, int), 10, 1);
        break;
      case 'u':
        print_num(&out, va_arg(ap, unsigned int), 10, 0);
        break;
      case 'x':
        print_num(&out, va_arg(ap, unsigned int), 16, 0);
        break;
      case 'p':   // 指针输出为十六进制
        putchstd(&out, '0');
        putchstd(&out, 'x');
        print_num(&out, (uintptr_t)va_arg(ap, void *), 16, 0);
        break;
      case 's': {
        const char *s = va_arg(ap, const char *);
        if (!s) s = "(null)";
        while (*s) putchstd(&out, *s++);
        break;
      }
      case 'c':
        putchstd(&out, (char)va_arg(ap, int));
        break;
      case '%':
        putchstd(&out, '%');
        break;
      default:
        // 未知格式，直接输出
        putchstd(&out, '%');
        putchstd(&out, *fmt);
        break;
    }
  }

  *out = '\0';
  va_end(ap);
  return out - start; // 返回写入的长度
}

int snprintf(char *out, size_t n, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(out, n, fmt, ap);
  va_end(ap);
  return len;
}

int vsnprintf(char *out, size_t n, const char *fmt, va_list ap) {
  char *start = out;
  char *end = out + (n > 0 ? n - 1 : 0);  // 最多留 1 个字节给 '\0'

  for (; *fmt; fmt++) {
    if (*fmt != '%') {
      putchstd_n(&out, *fmt, end);
      continue;
    }

    fmt++;  // 跳过 '%'
    switch (*fmt) {
      case 'd':
        print_num_n(&out, va_arg(ap, int), 10, 1, end);
        break;
      case 'u':
        print_num_n(&out, va_arg(ap, unsigned int), 10, 0, end);
        break;
      case 'x':
        print_num_n(&out, va_arg(ap, unsigned int), 16, 0, end);
        break;
      case 'p': {
        putchstd_n(&out, '0', end);
        putchstd_n(&out, 'x', end);
        print_num_n(&out, (uintptr_t)va_arg(ap, void *), 16, 0, end);
        break;
      }
      case 's': {
        const char *s = va_arg(ap, const char *);
        if (!s) s = "(null)";
        while (*s) putchstd_n(&out, *s++, end);
        break;
      }
      case 'c':
        putchstd_n(&out, (char)va_arg(ap, int), end);
        break;
      case '%':
        putchstd_n(&out, '%', end);
        break;
      default:
        putchstd_n(&out, '%', end);
        putchstd_n(&out, *fmt, end);
        break;
    }
  }

  if (n > 0) *out = '\0';  // 确保结尾安全

  return out - start;  // 返回理论写入长度（不含 '\0'）
}

#endif
