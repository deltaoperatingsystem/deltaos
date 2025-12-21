#include <drivers/console.h>
#include <drivers/serial.h>
#include <stdarg.h>

enum output_mode {
    SERIAL,
    CONSOLE,
};

static enum output_mode mode = SERIAL;

void puts(const char *s) {
    switch (mode) {
        case SERIAL:
            serial_write(s);
            return;
        case CONSOLE:
            con_print(s);
            return;
        default: return;
    }
}

void putc(const char c) {
    switch (mode) {
        case SERIAL:
            serial_write_char(c);
            return;
        case CONSOLE:
            con_putc(c);
            return;
        default: return;
    }
}

void printf(const char *format, ...) {
    va_list args;
    va_start(args, format);

    char buffer[1024];
    char *out = buffer;
    char *limit = buffer + sizeof(buffer) - 1;
    const char *p = format;

    while (*p && out < limit) {
        if (*p == '%') {
            p++;
            int is_long = 0;
            if (*p == 'l') {
                is_long = 1;
                p++;
            }

            if (*p == 's') {
                const char *str = va_arg(args, const char *);
                if (!str) str = "(null)";
                while (*str && out < limit) {
                    *out++ = *str++;
                }
            }
            else if (*p == 'c') {
                *out++ = (char)va_arg(args, int);
            }
            else if (*p == 'd') {
                int64 num;
                if (is_long) num = va_arg(args, long);
                else num = va_arg(args, int);

                char tmp[21];
                int len = 0;

                if (num < 0) {
                    if (out < limit) *out++ = '-';
                    uint64 u = (num == INT64_MIN) ? (uint64)INT64_MAX + 1 : (uint64)-num;
                    if (u == 0 && num != 0) u = 0x8000000000000000ULL; //absolute min
                    while (u) {
                        tmp[len++] = (char)('0' + (u % 10));
                        u /= 10;
                    }
                } else if (num == 0) {
                    tmp[len++] = '0';
                } else {
                    uint64 u = (uint64)num;
                    while (u) {
                        tmp[len++] = (char)('0' + (u % 10));
                        u /= 10;
                    }
                }
                for (int i = len - 1; i >= 0 && out < limit; i--) {
                    *out++ = tmp[i];
                }
            } else if (*p == 'x' || *p == 'p') {
                uint64 num;
                if (*p == 'p') {
                    num = (uintptr)va_arg(args, void*);
                    if (out < limit - 2) {
                        *out++ = '0';
                        *out++ = 'x';
                    }
                } else {
                    if (is_long) num = va_arg(args, unsigned long);
                    else num = va_arg(args, unsigned int);
                }

                char tmp[17];
                int len = 0;

                if (num == 0) {
                    tmp[len++] = '0';
                } else {
                    while (num) {
                        int digit = num & 0xF;
                        tmp[len++] = (char)((digit < 10) ? ('0' + digit) : ('a' + (digit - 10)));
                        num >>= 4;
                    }
                }
                for (int i = len - 1; i >= 0 && out < limit; i--) {
                    *out++ = tmp[i];
                }
            }
            else if (*p == '%') {
                *out++ = '%';
            }
            else {
                if (out < limit - 1) {
                    *out++ = '%';
                    *out++ = *p;
                }
            }
        }
        else {
            *out++ = *p;
        }
        p++;
    }

    *out = '\0';
    va_end(args);
    puts(buffer);
}

void set_outmode(enum output_mode m) {
    mode = m;
}