#ifndef __SYSTEM_H
#define __SYSTEM_H

#include <types.h>
#include <sys/syscall.h>

//handle rights
#define RIGHT_NONE          0
#define RIGHT_DUPLICATE     (1 << 0)
#define RIGHT_TRANSFER      (1 << 1)
#define RIGHT_READ          (1 << 2)
#define RIGHT_WRITE         (1 << 3)
#define RIGHT_EXECUTE       (1 << 4)

//invalid handle sentinel
#define INVALID_HANDLE      (-1)

//process control
void exit(int code);
int64 getpid(void);
void yield(void);
int spawn(char *path, int argc, char **argv);

//capability-based object access
int32 get_obj(int32 parent, const char *path, uint32 rights);
int handle_read(int32 h, void *buf, int len);
int handle_write(int32 h, const void *buf, int len);
int handle_close(int32 h);

//channel IPC
int channel_create(int32 *ep0, int32 *ep1);
int channel_send(int32 ep, const void *data, int len);
int channel_recv(int32 ep, void *buf, int buflen);

#endif