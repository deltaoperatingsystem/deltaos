#ifndef __TYPES_H
#define __TYPES_H

//exact width integer types
typedef signed char         int8;
typedef unsigned char       uint8;
typedef signed short        int16;
typedef unsigned short      uint16;
typedef signed int          int32;
typedef unsigned int        uint32;
typedef signed long long    int64;
typedef unsigned long long  uint64;

typedef signed char         int8_t;
typedef unsigned char       uint8_t;
typedef signed short        int16_t;
typedef unsigned short      uint16_t;
typedef signed int          int32_t;
typedef unsigned int        uint32_t;
typedef signed long long    int64_t;
typedef unsigned long long  uint64_t;

//limits
#define INT8_MIN    (-128)
#define INT8_MAX    127
#define UINT8_MAX   255

#define INT16_MIN   (-32768)
#define INT16_MAX   32767
#define UINT16_MAX  65535

#define INT32_MIN   (-2147483647 - 1)
#define INT32_MAX   2147483647
#define UINT32_MAX  4294967295U

#define INT64_MIN   (-9223372036854775807LL - 1)
#define INT64_MAX   9223372036854775807LL
#define UINT64_MAX  18446744073709551615ULL

//common definitions
#define NULL ((void*)0)

typedef char bool;
#ifndef __bool_true_false_are_defined
#define __bool_true_false_are_defined 1
#ifndef __cplusplus
#define true  1
#define false 0
#endif
#endif

//architecture info
#define ARCH_BITS     64
#define ARCH_PTR_SIZE 8

//pointer-sized integers (64-bit on amd64)
typedef int64  intptr;
typedef uint64 uintptr;
typedef int64  intptr_t;
typedef uint64 uintptr_t;

//size types (64-bit on amd64)
typedef uint64 size;
typedef uint64 size_t;
typedef int64  ssize;
typedef int64  ssize_t;

//native word types (64-bit on amd64)
typedef int64  word;
typedef uint64 uword;

//maximum-width integer types
typedef int64  intmax;
typedef uint64 uintmax;
typedef int64  intmax_t;
typedef uint64 uintmax_t;

//pointer-sized limits
#define INTPTR_MIN  INT64_MIN
#define INTPTR_MAX  INT64_MAX
#define UINTPTR_MAX UINT64_MAX
#define SIZE_MAX    UINT64_MAX
#define SSIZE_MAX   INT64_MAX

typedef struct stat {
    uint32 type;
    size   size;
    uint64 ctime;
    uint64 mtime;
    uint64 atime;
    uint32 width;
    uint32 height;
    uint32 pitch;
} stat_t;

#endif