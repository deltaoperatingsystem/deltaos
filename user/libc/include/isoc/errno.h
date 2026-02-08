#ifndef _ERRNO_H
#define _ERRNO_H

extern int *__errno_location(void);
#define errno (*__errno_location())

#define ENOENT 2
#define EISDIR 21
#define EINVAL 22

#endif
