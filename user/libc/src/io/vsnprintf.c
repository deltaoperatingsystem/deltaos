#include <io.h>
#include <types.h>
#include <args.h>

typedef struct {
    char *buf;
    size pos;
    size max;
} print_ctx_t;

static void ctx_putc(print_ctx_t *ctx, char c) {
    if (ctx->buf) {
        if (ctx->pos < ctx->max - 1) {
            ctx->buf[ctx->pos] = c;
        }
        ctx->pos++;
    }
}

static int do_printf(print_ctx_t *ctx, const char *format, va_list args) {
    const char *p = format;

    while (*p) {
        if (*p == '%') {
            p++;
            if (!*p) {
                ctx_putc(ctx, '%');
                break;
            }
            
            int zero_pad = 0;
            int left_align = 0;
            while (*p == '0' || *p == '-') {
                if (*p == '0') zero_pad = 1;
                if (*p == '-') left_align = 1;
                p++;
            }
            if (left_align) zero_pad = 0;
            
            int width = 0;
            if (*p == '*') {
                width = va_arg(args, int);
                p++;
                if (width < 0) {
                    left_align = 1;
                    width = -width;
                }
            } else {
                while (*p >= '0' && *p <= '9') {
                    width = width * 10 + (*p - '0');
                    p++;
                }
            }
            if (left_align) zero_pad = 0;

            int precision = -1;
            if (*p == '.') {
                p++;
                precision = 0;
                if (*p == '*') {
                    precision = va_arg(args, int);
                    p++;
                } else {
                    while (*p >= '0' && *p <= '9') {
                        precision = precision * 10 + (*p - '0');
                        p++;
                    }
                }
            }
            
            int is_long = 0;
            int is_size = 0;
            if (*p == 'l') {
                is_long = 1;
                p++;
                if (*p == 'l') p++;
            } else if (*p == 'z') {
                is_size = 1;
                p++;
            }

            if (*p == 's') {
                const char *str = va_arg(args, const char *);
                if (!str) str = "(null)";
                int len = 0;
                const char *s = str;
                while (*s++) len++;
                
                if (precision >= 0 && precision < len) len = precision;
                
                if (!left_align && width > len) {
                    for (int i = 0; i < width - len; i++) ctx_putc(ctx, ' ');
                }
                s = str;
                for (int i = 0; i < len; i++) ctx_putc(ctx, *s++);
                if (left_align && width > len) {
                    for (int i = 0; i < width - len; i++) ctx_putc(ctx, ' ');
                }
            } else if (*p == 'c') {
                char c = (char)va_arg(args, int);
                if (!left_align && width > 1) {
                    for (int i = 0; i < width - 1; i++) ctx_putc(ctx, ' ');
                }
                ctx_putc(ctx, c);
                if (left_align && width > 1) {
                    for (int i = 0; i < width - 1; i++) ctx_putc(ctx, ' ');
                }
            } else if (*p == 'd' || *p == 'i') {
                intmax num;
                if (is_long || is_size) num = va_arg(args, long);
                else num = va_arg(args, int);

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

                if (u == 0 && precision == 0) {
                    len = 0; //"%.0d" with 0 prints nothing
                } else if (u == 0) {
                    tmp[len++] = '0';
                } else {
                    while (u) {
                        tmp[len++] = (char)('0' + (u % 10));
                        u /= 10;
                    }
                }
                
                int prec_pad = (precision > len) ? precision - len : 0;
                int total = len + neg + prec_pad;
                int pad = (width > total) ? width - total : 0;
                
                if (!left_align && !zero_pad && pad > 0) {
                    for (int i = 0; i < pad; i++) ctx_putc(ctx, ' ');
                }
                if (neg) ctx_putc(ctx, '-');
                if (!left_align && zero_pad && pad > 0 && precision < 0) {
                    for (int i = 0; i < pad; i++) ctx_putc(ctx, '0');
                }
                while (prec_pad--) ctx_putc(ctx, '0');
                for (int i = len - 1; i >= 0; i--) ctx_putc(ctx, tmp[i]);
                if (left_align && pad > 0) {
                    for (int i = 0; i < pad; i++) ctx_putc(ctx, ' ');
                }
            } else if (*p == 'u') {
                uintmax num;
                if (is_long || is_size) num = va_arg(args, unsigned long);
                else num = va_arg(args, unsigned int);

                char tmp[32];
                int len = 0;
                if (num == 0 && precision == 0) {
                    len = 0;
                } else if (num == 0) {
                    tmp[len++] = '0';
                } else {
                    while (num) {
                        tmp[len++] = (char)('0' + (num % 10));
                        num /= 10;
                    }
                }
                
                int prec_pad = (precision > len) ? precision - len : 0;
                int total = len + prec_pad;
                int pad = (width > total) ? width - total : 0;
                if (!left_align && !zero_pad && pad > 0) {
                    for (int i = 0; i < pad; i++) ctx_putc(ctx, ' ');
                }
                if (!left_align && zero_pad && pad > 0 && precision < 0) {
                    for (int i = 0; i < pad; i++) ctx_putc(ctx, '0');
                }
                while (prec_pad--) ctx_putc(ctx, '0');
                for (int i = len - 1; i >= 0; i--) ctx_putc(ctx, tmp[i]);
                if (left_align && pad > 0) {
                    for (int i = 0; i < pad; i++) ctx_putc(ctx, ' ');
                }
            } else if (*p == 'x' || *p == 'X' || *p == 'p' || *p == 'P') {
                uintmax num;
                int prefix = 0;
                char hexcase = (*p == 'X' || *p == 'P') ? 'A' : 'a';
                
                if (*p == 'p') {
                    num = (uintptr)va_arg(args, void*);
                    prefix = 1;
                } else if (is_long || is_size) {
                    num = va_arg(args, unsigned long);
                } else {
                    num = va_arg(args, unsigned int);
                }

                char tmp[32];
                int len = 0;

                if (num == 0 && precision == 0) {
                    len = 0;
                } else if (num == 0) {
                    tmp[len++] = '0';
                } else {
                    while (num) {
                        int digit = num & 0xF;
                        tmp[len++] = (char)((digit < 10) ? ('0' + digit) : (hexcase + (digit - 10)));
                        num >>= 4;
                    }
                }
                
                int prec_pad = (precision > len) ? precision - len : 0;
                int total = len + (prefix ? 2 : 0) + prec_pad;
                int pad = (width > total) ? width - total : 0;
                
                if (!left_align && !zero_pad && pad > 0) {
                    for (int i = 0; i < pad; i++) ctx_putc(ctx, ' ');
                }
                if (prefix) {
                    ctx_putc(ctx, '0');
                    ctx_putc(ctx, 'x');
                }
                if (!left_align && zero_pad && pad > 0 && precision < 0) {
                    for (int i = 0; i < pad; i++) ctx_putc(ctx, '0');
                }
                while (prec_pad--) ctx_putc(ctx, '0');
                for (int i = len - 1; i >= 0; i--) ctx_putc(ctx, tmp[i]);
                if (left_align && pad > 0) {
                    for (int i = 0; i < pad; i++) ctx_putc(ctx, ' ');
                }
            } else if (*p == 'f') {
                double f = va_arg(args, double);
                int integer = (int)f;
                double frac = f - integer;
                
                if (f < 0 && integer == 0) ctx_putc(ctx, '-');
                
                //integer part (re-use logic for %d)
                char tmp[32];
                int len = 0;
                int neg = 0;
                uintmax u;
                if (integer < 0) { neg = 1; u = (uintmax)-(intmax)integer; }
                else u = (uintmax)integer;
                if (u == 0) tmp[len++] = '0';
                else while(u) { tmp[len++] = (char)('0' + (u % 10)); u /= 10; }
                if (neg) ctx_putc(ctx, '-');
                for (int i = len - 1; i >= 0; i--) ctx_putc(ctx, tmp[i]);

                if (frac < 0) frac = -frac;
                ctx_putc(ctx, '.');
                int digits = (precision >= 0) ? precision : 6;
                while (digits--) {
                    frac *= 10.0;
                    int digit = (int)frac;
                    ctx_putc(ctx, '0' + digit);
                    frac -= digit;
                }
            } else if (*p == '%') {
                ctx_putc(ctx, '%');
            } else {
                ctx_putc(ctx, '%');
                ctx_putc(ctx, *p);
            }
        } else {
            ctx_putc(ctx, *p);
        }
        p++;
    }

    return (int)ctx->pos;
}

int vsnprintf(char *buf, size n, const char *format, va_list args) {
    if (n == 0) return 0;
    print_ctx_t ctx = { .buf = buf, .pos = 0, .max = n };
    int ret = do_printf(&ctx, format, args);
    if (ctx.pos < n) {
        buf[ctx.pos] = '\0';
    } else {
        buf[n - 1] = '\0';
    }
    return ret;
}

int snprintf(char *buf, size n, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int ret = vsnprintf(buf, n, format, args);
    va_end(args);
    return ret;
}
