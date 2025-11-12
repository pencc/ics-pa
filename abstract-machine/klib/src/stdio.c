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

int printf(const char *fmt, ...) {
  panic("Not implemented");
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
  panic("Not implemented");
}

int vsnprintf(char *out, size_t n, const char *fmt, va_list ap) {
  panic("Not implemented");
}

#endif
