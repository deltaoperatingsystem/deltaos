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

    const char *p = format;

    while (*p) {
        if (*p == '%') {
            p++;
            int is_long = 0;
            int is_size = 0;
            if (*p == 'l') {
                is_long = 1;
                p++;
            } else if (*p == 'z') {
                is_size = 1;
                p++;
            }

            if (*p == 's') {
                const char *str = va_arg(args, const char *);
                if (!str) str = "(null)";
                while (*str) {
                    putc(*str++);
                }
            }
            else if (*p == 'c') {
                putc((char)va_arg(args, int));
            }
            else if (*p == 'd') {
                intmax num;
                if (is_long || is_size) num = va_arg(args, long);
                else num = va_arg(args, int);

                char tmp[32];
                int len = 0;

                uintmax u;
                if (num < 0) {
                    putc('-');
                    u = (uintmax)-(intmax)num;
                } else {
                    u = (uintmax)num;
                }

                if (u == 0) {
                    tmp[len++] = '0';
                } else {
                    while (u) {
                        tmp[len++] = (char)('0' + (u % 10));
                        u /= 10;
                    }
                }
                for (int i = len - 1; i >= 0; i--) {
                    putc(tmp[i]);
                }
            } else if (*p == 'u') {
                uintmax num;
                if (is_long || is_size) num = va_arg(args, unsigned long);
                else num = va_arg(args, unsigned int);

                char tmp[32];
                int len = 0;
                if (num == 0) {
                    tmp[len++] = '0';
                } else {
                    while (num) {
                        tmp[len++] = (char)('0' + (num % 10));
                        num /= 10;
                    }
                }
                for (int i = len - 1; i >= 0; i--) {
                    putc(tmp[i]);
                }
            } else if (*p == 'x' || *p == 'p') {
                uintmax num;
                if (*p == 'p') {
                    num = (uintptr)va_arg(args, void*);
                    putc('0');
                    putc('x');
                } else {
                    if (is_long || is_size) num = va_arg(args, unsigned long);
                    else num = va_arg(args, unsigned int);
                }

                char tmp[32];
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
                for (int i = len - 1; i >= 0; i--) {
                    putc(tmp[i]);
                }
            }
            else if (*p == '%') {
                putc('%');
            }
            else {
                putc('%');
                putc(*p);
            }
        }
        else {
            putc(*p);
        }
        p++;
    }

    va_end(args);
}

void set_outmode(enum output_mode m) {
    mode = m;
}