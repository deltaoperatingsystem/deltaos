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
int32 handle_dup(int32 h, uint32 new_rights);

#define HANDLE_SEEK_SET     0
#define HANDLE_SEEK_OFF     1
#define HANDLE_SEEK_END     2
int handle_seek(int32 h, size offset, int mode);

//channel IPC
int channel_create(int32 *ep0, int32 *ep1);
int channel_send(int32 ep, const void *data, int len);
int channel_recv(int32 ep, void *buf, int buflen);
int channel_try_recv(int32 ep, void *buf, int buflen);

//extended channel recv with sender info
typedef struct {
    uint64 data_len;     //actual bytes of data received
    uint32 handle_count; //number of handles received
    uint32 sender_pid;   //PID of the process that sent this message (0 if kernel)
} channel_recv_result_t;

int channel_recv_msg(int32 ep, void *data_buf, int data_len,
                     int32 *handles_buf, uint32 handles_len,
                     channel_recv_result_t *result);

//virtual memory objects
int32 vmo_create(uint64 size, uint32 flags, uint32 rights);
int vmo_read(int32 h, void *buf, uint64 len, uint64 offset);
int vmo_write(int32 h, const void *buf, uint64 len, uint64 offset);
void *vmo_map(int32 h, void *vaddr_hint, uint64 offset, uint64 len, uint32 flags);
int vmo_unmap(void *vaddr, uint64 len);

//namespace operations
int ns_register(const char *path, int32 h);

//metadata
int stat(const char *path, stat_t *st);

#endif