#include "stdio.h"
#include <stdint.h>
#include <string.h>

static void append_char(char *str, size_t size, size_t *pos, char c) {
    if (*pos < size - 1) {
        str[*pos] = c;
    }
    (*pos)++;
}

static void append_number(char *str, size_t size, size_t *pos, uint64_t val, int base, int uppercase) {
    char buf[64];
    int i = 0;
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (val == 0) {
        append_char(str, size, pos, '0');
        return;
    }

    while (val > 0) {
        buf[i++] = digits[val % base];
        val /= base;
    }

    while (i > 0) {
        append_char(str, size, pos, buf[--i]);
    }
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    size_t pos = 0;
    const char *p = format;

    while (*p) {
        if (*p == '%' && *(p + 1)) {
            p++;
            switch (*p) {
                case 'c': {
                    char c = (char)va_arg(ap, int);
                    append_char(str, size, &pos, c);
                    break;
                }
                case 's': {
                    const char *s = va_arg(ap, const char *);
                    if (!s) s = "(null)";
                    while (*s) {
                        append_char(str, size, &pos, *s++);
                    }
                    break;
                }
                case 'd':
                case 'i': {
                    int64_t val = va_arg(ap, int64_t);
                    if (val < 0) {
                        append_char(str, size, &pos, '-');
                        val = -val;
                    }
                    append_number(str, size, &pos, (uint64_t)val, 10, 0);
                    break;
                }
                case 'u': {
                    uint64_t val = va_arg(ap, uint64_t);
                    append_number(str, size, &pos, val, 10, 0);
                    break;
                }
                case 'x': {
                    uint64_t val = va_arg(ap, uint64_t);
                    append_number(str, size, &pos, val, 16, 0);
                    break;
                }
                case 'X': {
                    uint64_t val = va_arg(ap, uint64_t);
                    append_number(str, size, &pos, val, 16, 1);
                    break;
                }
                case 'p': {
                    uint64_t val = (uint64_t)va_arg(ap, void *);
                    append_char(str, size, &pos, '0');
                    append_char(str, size, &pos, 'x');
                    append_number(str, size, &pos, val, 16, 0);
                    break;
                }
                case '%': {
                    append_char(str, size, &pos, '%');
                    break;
                }
                default:
                    append_char(str, size, &pos, '%');
                    append_char(str, size, &pos, *p);
                    break;
            }
        } else {
            append_char(str, size, &pos, *p);
        }
        p++;
    }

    if (size > 0) {
        if (pos < size) {
            str[pos] = '\0';
        } else {
            str[size - 1] = '\0';
        }
    }

    return (int)pos;
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vsnprintf(str, size, format, ap);
    va_end(ap);
    return ret;
}
