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
#define RIGHT_MAP           (1 << 5)
#define RIGHT_GET_INFO      (1 << 6)

//VMO flags
#define VMO_FLAG_NONE       0
#define VMO_FLAG_RESIZABLE  (1 << 0)

typedef int32 handle_t;

//invalid handle sentinel
#define INVALID_HANDLE      (-1)

//process control
void exit(int code);
int64 getpid(void);
void yield(void);
int spawn(char *path, int argc, char **argv);
int wait(int pid);
uint64 get_ticks(void);

//capability-based process creation (Zircon-style)
int32 process_create(const char *name);              //create suspended process, returns handle
int handle_grant(int32 proc_h, int32 local_h, uint32 rights);  //inject handle into child
int process_start(int32 proc_h, uint64 entry, uint64 stack);   //start first thread

//capability-based object access
handle_t get_obj(handle_t parent, const char *path, uint32 rights);
int handle_read(handle_t h, void *buf, int len);
int handle_write(handle_t h, const void *buf, int len);
int handle_close(handle_t h);
handle_t handle_dup(handle_t h, uint32 new_rights);

#define HANDLE_SEEK_SET     0
#define HANDLE_SEEK_OFF     1
#define HANDLE_SEEK_END     2
int handle_seek(handle_t h, size offset, int mode);

//channel IPC
int channel_create(handle_t *ep0, handle_t *ep1);
int channel_send(handle_t ep, const void *data, int len);
int channel_recv(handle_t ep, void *buf, int buflen);
int channel_try_recv(handle_t ep, void *buf, int buflen);

//directory entry structure
#define DIRENT_NAME_MAX 64
typedef struct {
    char name[DIRENT_NAME_MAX];  //entry name
    uint32 type;                  //file type
} dirent_t;

//filesystem types
#define FS_TYPE_FILE    1
#define FS_TYPE_DIR     2

//extended channel recv with sender info
typedef struct {
    uint64 data_len;     //actual bytes of data received
    uint32 handle_count; //number of handles received
    uint32 sender_pid;   //PID of the process that sent this message (0 if kernel)
} channel_recv_result_t;

int channel_recv_msg(handle_t ep, void *data_buf, int data_len,
                     int32 *handles_buf, uint32 handles_len,
                     channel_recv_result_t *result);
int channel_try_recv_msg(handle_t ep, void *data_buf, int data_len,
                     int32 *handles_buf, uint32 handles_len,
                     channel_recv_result_t *result);

//virtual memory objects
handle_t vmo_create(uint64 size, uint32 flags, uint32 rights);
int vmo_read(handle_t h, void *buf, uint64 len, uint64 offset);
int vmo_write(handle_t h, const void *buf, uint64 len, uint64 offset);
void *vmo_map(handle_t h, void *vaddr_hint, uint64 offset, uint64 len, uint32 flags);
int vmo_unmap(void *vaddr, uint64 len);
int vmo_resize(handle_t h, uint64 new_size);

//namespace operations
int ns_register(const char *path, handle_t h);

//metadata
int stat(const char *path, stat_t *st);
int fstat(handle_t h, stat_t *st);

//directory reading
int readdir(handle_t h, dirent_t *entries, uint32 count, uint32 *index);

//current working directory
int chdir(const char *path);
int getcwd(char *buf, size size);

//object info
typedef enum {
    OBJ_INFO_NONE = 0,
    OBJ_INFO_PROCESS_BASIC = 1, //process_info_basic_t
    OBJ_INFO_THREAD_STATS = 2,  //thread_stats_t
    OBJ_INFO_KMEM_STATS = 3,    //kmem_stats_t (requires system handle)
    OBJ_INFO_TIME_STATS = 4,    //time_stats_t (requires system handle)
    OBJ_INFO_SYSTEM_STATS = 5   //system_stats_t (requires system handle)
} object_info_topic_t;

typedef struct {
    uint32 pid;
    uint32 parent_pid;
    char name[32];
    uint32 status;
    uint64 memory_usage;
} process_info_basic_t;

typedef struct {
    uint32 tid;
    uint32 state;
    uint64 cpu_time_ns;
    uint32 priority;
} thread_stats_t;

typedef struct {
    uint64 total_ram;
    uint64 free_ram;
    uint64 used_ram;
    uint64 heap_used;
    uint64 heap_free;
} kmem_stats_t;

typedef struct {
    uint64 uptime_ns;
    uint64 ticks;
    uint32 rtc_time; //seconds since 2000-01-01
} time_stats_t;

typedef struct {
    uint32 cpu_count;       //number of online CPUs
    uint32 cpu_freq_mhz;    //CPU frequency in MHz (approx)
    char os_name[32];       //OS name
    char os_version[16];    //OS version
    char arch[16];          //architecture name
    char cpu_vendor[16];    //CPU vendor (e.g. "GenuineIntel")
    char cpu_brand[48];     //CPU brand string (e.g. "Intel Core i7...")
} system_stats_t;

int object_get_info(handle_t h, uint32 topic, void *ptr, uint64 len);

#endif