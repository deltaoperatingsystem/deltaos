#if defined(__x86_64__) || defined(_M_X64)
#include <arch/amd64/io/port.h>
#elif defined(__i386__) || defined(_M_IX86)
#error Not implemented!
#elif defined(__aarch64__) || defined(_M_ARM64)
#error Not implemented!
#elif defined(__arm__) || defined(_M_ARM)
#error Not implemented!
#else
#error Unknown architecture!
#endif
