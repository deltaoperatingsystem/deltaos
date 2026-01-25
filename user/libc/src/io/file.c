#include <stdio.h>
#include <mem.h>
#include <system.h>
#include <string.h>
#include <args.h>

static FILE _stdout = { .handle = 1, .eof = false };
static FILE _stderr = { .handle = 1, .eof = false };
static FILE _stdin  = { .handle = 0, .eof = false };

FILE *stdout = &_stdout;
FILE *stderr = &_stderr;
FILE *stdin  = &_stdin;

FILE *fopen(const char *path, const char *mode) {
    uint32 rights = RIGHT_READ;
    if (strchr(mode, 'w')) rights |= RIGHT_WRITE;
    
    handle_t h = get_obj(INVALID_HANDLE, path, rights);
    if (h == INVALID_HANDLE) return NULL;
    
    FILE *f = malloc(sizeof(FILE));
    f->handle = h;
    f->eof = false;
    return f;
}

int fclose(FILE *f) {
    if (!f || f == stdout || f == stderr || f == stdin) return -1;
    handle_close(f->handle);
    free(f);
    return 0;
}

int fflush(FILE *f) {
    return 0;
}

size fread(void *ptr, size sz, size nmemb, FILE *f) {
    if (!f) return 0;
    int res = handle_read(f->handle, ptr, sz * nmemb);
    if (res <= 0) {
        f->eof = true;
        return 0;
    }
    return res / sz;
}

size fwrite(const void *ptr, size sz, size nmemb, FILE *f) {
    if (!f) return 0;
    int res = handle_write(f->handle, ptr, sz * nmemb);
    if (res <= 0) return 0;
    return res / sz;
}

int fseek(FILE *f, long offset, int whence) {
    if (!f) return -1;
    f->eof = false;
    return handle_seek(f->handle, (size)offset, whence);
}

long ftell(FILE *f) {
    if (!f) return -1;
    return (long)handle_seek(f->handle, 0, HANDLE_SEEK_OFF);
}

int feof(FILE *f) {
    return f ? (int)f->eof : 1;
}

int putchar(int c) {
    putc((char)c);
    return c;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    if (!f) return -1;
    char buf[1024];
    int total = vsnprintf(buf, sizeof(buf), fmt, ap);
    int written = (total < (int)sizeof(buf)) ? total : (int)sizeof(buf) - 1;
    return fwrite(buf, 1, written, f);
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int res = vfprintf(f, fmt, args);
    va_end(args);
    return res;
}

int remove(const char *pathname) {
    return __syscall1(SYS_REMOVE, (long)pathname);
}

static int64 sys_rename(const char *old, const char *new) {
    return -1; //Not implemented in kernel yet
}

int rename(const char *oldpath, const char *newpath) {
    return -1;
}

int vsscanf(const char *str, const char *format, va_list args) {
    int count = 0;
    const char *p = format;
    const char *s = str;

    while (*p && *s) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
            p++;
            continue;
        }
        if (*p == '%') {
            p++;
            int width = 0;
            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p - '0');
                p++;
            }

            if (*p == 'd' || *p == 'u' || *p == 'i' || *p == 'x' || *p == 'p') {
                void *dest = va_arg(args, void *);
                long val = 0;
                int sign = 1;
                while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
                if (*s == '-') { 
                    if (*p == 'u' || *p == 'p' || *p == 'x') break; 
                    sign = -1; s++; 
                }

                int base = 10;
                if (*p == 'x' || *p == 'p') {
                    base = 16;
                    if (*s == '0' && (*(s+1) == 'x' || *(s+1) == 'X')) s += 2;
                } else if (*p == 'i') {
                    if (*s == '0') {
                        if (*(s+1) == 'x' || *(s+1) == 'X') { base = 16; s += 2; }
                        else { base = 8; s++; }
                    }
                }

                int digits = 0;
                while (1) {
                    int d = -1;
                    if (*s >= '0' && *s <= '9') d = *s - '0';
                    else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
                    else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
                    
                    if (d == -1 || d >= base) break;
                    val = val * base + d;
                    s++;
                    digits++;
                    if (width && digits >= width) break;
                }
                if (digits > 0) {
                    if (*p == 'p' || *p == 'x' || *p == 'u') *(unsigned long *)dest = (unsigned long)val;
                    else *(long *)dest = val * sign;
                    count++;
                } else break;
            } else if (*p == 's') {
                char *dest = va_arg(args, char *);
                while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
                int chars = 0;
                while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') {
                    *dest++ = *s++;
                    chars++;
                    if (width && chars >= width) break;
                }
                *dest = '\0';
                if (chars > 0) count++;
                else break;
            } else if (*p == '[') {
                p++;
                bool negate = false;
                if (*p == '^') { negate = true; p++; }
                char set[256] = {0};
                while (*p && *p != ']') {
                    set[(unsigned char)*p] = 1;
                    p++;
                }

                char *dest = va_arg(args, char *);
                int chars = 0;
                while (*s) {
                    bool match = set[(unsigned char)*s];
                    if (negate) match = !match;
                    if (!match) break;
                    *dest++ = *s++;
                    chars++;
                    if (width && chars >= width) break;
                }
                *dest = '\0';
                if (chars > 0) count++;
                else break;
            } else if (*p == '%') {
                if (*s == '%') s++;
                else break;
            }
            p++;
        } else {
            if (*p == *s) {
                p++;
                s++;
            } else {
                break;
            }
        }
    }
    return count;
}

int sscanf(const char *str, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int count = vsscanf(str, format, args);
    va_end(args);
    return count;
}

int fscanf(FILE *f, const char *format, ...) {
    if (!f) return -1;
    
    char line[512];
    size pos = 0;
    while (pos < sizeof(line) - 1) {
        char c;
        if (handle_read(f->handle, &c, 1) <= 0) {
            f->eof = true;
            break;
        }
        line[pos++] = c;
        if (c == '\n') break;
    }
    line[pos] = '\0';
    if (pos == 0 && f->eof) return -1;

    va_list args;
    va_start(args, format);
    int count = vsscanf(line, format, args);
    va_end(args);
    return count;
}
