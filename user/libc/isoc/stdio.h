#ifndef __STDIO_H
#define __STDIO_H

#include <types.h>
#include <system.h>
#include <io.h>

typedef struct {
    handle_t handle;
    bool eof;
} FILE;

#define SEEK_SET HANDLE_SEEK_SET
#define SEEK_CUR HANDLE_SEEK_OFF
#define SEEK_END HANDLE_SEEK_END

extern FILE *stdout;
extern FILE *stderr;
extern FILE *stdin;

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
int fflush(FILE *f);
size fread(void *ptr, size sz, size nmemb, FILE *f);
size fwrite(const void *ptr, size sz, size nmemb, FILE *f);
int fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);
int feof(FILE *f);

int putchar(int c);
int fprintf(FILE *f, const char *fmt, ...);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int remove(const char *pathname);
int rename(const char *oldpath, const char *newpath);
int fscanf(FILE *f, const char *fmt, ...);
int sscanf(const char *str, const char *fmt, ...);

#endif
