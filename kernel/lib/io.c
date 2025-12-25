#include <drivers/console.h>
#include <drivers/serial.h>
#include <stdarg.h>
#include <arch/types.h>

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

//output context for vsnprintf core
typedef struct {
    char *buf;      //NULL for console/serial output
    size pos;
    size max;
} print_ctx_t;

static void ctx_putc(print_ctx_t *ctx, char c) {
    if (ctx->buf) {
        //buffer output
        if (ctx->pos < ctx->max - 1) {
            ctx->buf[ctx->pos] = c;
        }
        ctx->pos++;
    } else {
        //direct output
        putc(c);
    }
}

//core printf implementation
static int do_printf(print_ctx_t *ctx, const char *format, va_list args) {
    const char *p = format;

    while (*p) {
        if (*p == '%') {
            p++;
            
            //flags
            int zero_pad = 0;
            int left_align = 0;
            while (*p == '0' || *p == '-') {
                if (*p == '0') zero_pad = 1;
                if (*p == '-') left_align = 1;
                p++;
            }
            if (left_align) zero_pad = 0;  //- overrides 0
            
            //width
            int width = 0;
            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p - '0');
                p++;
            }
            
            //length modifiers
            int is_long = 0;
            int is_size = 0;
            if (*p == 'l') {
                is_long = 1;
                p++;
                if (*p == 'l') p++;  //%ll
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
                
                //right-align padding
                if (!left_align && width > len) {
                    for (int i = 0; i < width - len; i++) ctx_putc(ctx, ' ');
                }
                s = str;
                while (*s) ctx_putc(ctx, *s++);
                //left-align padding
                if (left_align && width > len) {
                    for (int i = 0; i < width - len; i++) ctx_putc(ctx, ' ');
                }
            }
            else if (*p == 'c') {
                char c = (char)va_arg(args, int);
                if (!left_align && width > 1) {
                    for (int i = 0; i < width - 1; i++) ctx_putc(ctx, ' ');
                }
                ctx_putc(ctx, c);
                if (left_align && width > 1) {
                    for (int i = 0; i < width - 1; i++) ctx_putc(ctx, ' ');
                }
            }
            else if (*p == 'd' || *p == 'i') {
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

                if (u == 0) {
                    tmp[len++] = '0';
                } else {
                    while (u) {
                        tmp[len++] = (char)('0' + (u % 10));
                        u /= 10;
                    }
                }
                
                int total = len + neg;
                int pad = (width > total) ? width - total : 0;
                
                if (!left_align && !zero_pad && pad > 0) {
                    for (int i = 0; i < pad; i++) ctx_putc(ctx, ' ');
                }
                if (neg) ctx_putc(ctx, '-');
                if (!left_align && zero_pad && pad > 0) {
                    for (int i = 0; i < pad; i++) ctx_putc(ctx, '0');
                }
                for (int i = len - 1; i >= 0; i--) ctx_putc(ctx, tmp[i]);
                if (left_align && pad > 0) {
                    for (int i = 0; i < pad; i++) ctx_putc(ctx, ' ');
                }
            } 
            else if (*p == 'u') {
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
                
                int pad = (width > len) ? width - len : 0;
                if (!left_align && !zero_pad && pad > 0) {
                    for (int i = 0; i < pad; i++) ctx_putc(ctx, ' ');
                }
                if (!left_align && zero_pad && pad > 0) {
                    for (int i = 0; i < pad; i++) ctx_putc(ctx, '0');
                }
                for (int i = len - 1; i >= 0; i--) ctx_putc(ctx, tmp[i]);
                if (left_align && pad > 0) {
                    for (int i = 0; i < pad; i++) ctx_putc(ctx, ' ');
                }
            } 
            else if (*p == 'x' || *p == 'X' || *p == 'p') {
                uintmax num;
                int prefix = 0;
                char hexcase = (*p == 'X') ? 'A' : 'a';
                
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

                if (num == 0) {
                    tmp[len++] = '0';
                } else {
                    while (num) {
                        int digit = num & 0xF;
                        tmp[len++] = (char)((digit < 10) ? ('0' + digit) : (hexcase + (digit - 10)));
                        num >>= 4;
                    }
                }
                
                int total = len + (prefix ? 2 : 0);
                int pad = (width > total) ? width - total : 0;
                
                if (!left_align && !zero_pad && pad > 0) {
                    for (int i = 0; i < pad; i++) ctx_putc(ctx, ' ');
                }
                if (prefix) {
                    ctx_putc(ctx, '0');
                    ctx_putc(ctx, 'x');
                }
                if (!left_align && zero_pad && pad > 0) {
                    for (int i = 0; i < pad; i++) ctx_putc(ctx, '0');
                }
                for (int i = len - 1; i >= 0; i--) ctx_putc(ctx, tmp[i]);
                if (left_align && pad > 0) {
                    for (int i = 0; i < pad; i++) ctx_putc(ctx, ' ');
                }
            } else if (*p == 'X' || *p == 'P') {
                uintmax num;
                if (*p == 'P') {
                    num = (uintptr)va_arg(args, void*);
                    putc('0');
                    putc('x');
                } else if (is_long || is_size) {
                    num = va_arg(args, unsigned long);
                } else {
                    num = va_arg(args, unsigned int);
                }

                char tmp[32];
                int len = 0;

                if (num == 0) {
                    tmp[len++] = '0';
                } else {
                    while (num) {
                        int digit = num & 0xF;
                        tmp[len++] = (char)((digit < 10) ? ('0' + digit) : ('A' + (digit - 10)));
                        num >>= 4;
                    }
                }
                for (int i = len - 1; i >= 0; i--) {
                    putc(tmp[i]);
                }
            }
            else if (*p == '%') {
                ctx_putc(ctx, '%');
            }
            else {
                ctx_putc(ctx, '%');
                ctx_putc(ctx, *p);
            }
        }
        else {
            ctx_putc(ctx, *p);
        }
        p++;
    }

    return (int)ctx->pos;
}

void printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    print_ctx_t ctx = { .buf = NULL, .pos = 0, .max = 0 };
    do_printf(&ctx, format, args);
    va_end(args);
}

int vsnprintf(char *buf, size n, const char *format, va_list args) {
    if (n == 0) return 0;
    print_ctx_t ctx = { .buf = buf, .pos = 0, .max = n };
    int ret = do_printf(&ctx, format, args);
    //null terminate
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

void set_outmode(enum output_mode m) {
    mode = m;
}