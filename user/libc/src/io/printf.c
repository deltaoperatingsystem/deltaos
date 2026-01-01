#include <sys/syscall.h>
#include <types.h>
#include <args.h>
#include <io.h>

void printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    const char *p = fmt;
    while (*p) {
        if (*p != '%') {
            putc(*p++);
            continue;
        }

        p++;
        if (*p == 'd') {
            int num = va_arg(args, int);

            char tmp[32];
            int len = 0;
            int neg = 0;

            uintmax u;
            if (num < 0) {
                neg = 1;
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
            
            if (neg) putc('-');
            for (int i = len - 1; i >= 0; i--) putc(tmp[i]);
        } else if (*p == 'f') {
            double f = va_arg(args, double);
            int integer = (int)f;
            double frac = f - integer;
            if (f < 0 && integer == 0) putc('-');

            // print the integer part
            {
                int num = integer;

                char tmp[32];
                int len = 0;
                int neg = 0;

                uintmax u;
                if (num < 0) {
                    neg = 1;
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
                
                if (neg) putc('-');
                for (int i = len - 1; i >= 0; i--) putc(tmp[i]);
            }

            if (frac < 0) frac = -frac;

            // print the fractional part
            {
                putc('.');

                int digits = 10; // 10 degrees of accuracy
                while (digits--) {
                    frac *= 10.0;
                    int digit = (int)frac;
                    putc('0' + digit);
                    frac -= digit;
                }
            }
        } else if (*p == 'c') {
            putc((char)va_arg(args, int));
        } else if (*p == 's') {
            const char *str = va_arg(args, const char *);
            if (!str) str = "(null)";
            puts(str);
        }

        p++;
    }

    va_end(args);
}