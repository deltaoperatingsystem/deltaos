#ifndef SYSCALL_SYSCALL_H
#define SYSCALL_SYSCALL_H

#include <arch/types.h>
#include <obj/handle.h>
#include <fs/fs.h>

//process management (SYS_INIT to 31)
#define SYS_EXIT            0
#define SYS_GETPID          1
#define SYS_YIELD           2
#define SYS_SPAWN           4   //spawns a new process
#define SYS_WAIT            47  //wait for process to exit
#define SYS_PROCESS_CREATE  50  //create suspended process, returns handle
#define SYS_HANDLE_GRANT    51  //inject handle into child process
#define SYS_PROCESS_START   52  //start initial thread in process

//object/handle management (32-41)
#define SYS_GET_OBJ         5   //get object from namespace
#define SYS_HANDLE_CLOSE    32
#define SYS_HANDLE_DUP      33
#define SYS_NS_REGISTER     42  //register handle in namespace

//ipc: channels (44-49)
#define SYS_CHANNEL_CREATE  34
#define SYS_CHANNEL_SEND    35
#define SYS_CHANNEL_RECV    36
#define SYS_CHANNEL_TRY_RECV 44  //non-blocking channel receive
#define SYS_CHANNEL_RECV_MSG 45  //receive with handles
#define SYS_CHANNEL_TRY_RECV_MSG 46 //non-blocking recv_msg

//memory: VMOs (53-56)
#define SYS_VMO_CREATE      37
#define SYS_VMO_READ        38
#define SYS_VMO_WRITE       39
#define SYS_VMO_MAP         40  //map VMO into address space
#define SYS_VMO_UNMAP       41  //unmap from address space
#define SYS_VMO_RESIZE      53  //resize a VMO

//filesystem (57-60)
#define SYS_STAT            43  //get file status by path
#define SYS_FSTAT           60  //get file status by handle
#define SYS_READDIR         54  //read directory entries
#define SYS_CHDIR           55  //change current working directory
#define SYS_GETCWD          56  //get current working directory
#define SYS_MKDIR           58  //create directory
#define SYS_REMOVE          59  //remove file or directory
#define SYS_HANDLE_READ     6   //read from handle
#define SYS_HANDLE_WRITE    7   //write to handle
#define SYS_HANDLE_SEEK     8   //seek to an offset in a handle

//misc/system
#define SYS_DEBUG_WRITE     3
#define SYS_GET_TICKS       57  //get timer ticks since boot
#define SYS_REBOOT          61  //reboot the system
#define SYS_SHUTDOWN        62  //shutdown the system

//object info (63)
#define SYS_OBJECT_GET_INFO 63

#define SYS_MAX             256

//object info topics
typedef enum {
    OBJ_INFO_NONE = 0,
    OBJ_INFO_PROCESS_BASIC = 1, //process_info_basic_t
    OBJ_INFO_THREAD_STATS = 2,  //thread_stats_t
    OBJ_INFO_KMEM_STATS = 3,    //kmem_stats_t (requires system handle)
    OBJ_INFO_TIME_STATS = 4,    //time_stats_t (requires system handle)
    OBJ_INFO_SYSTEM_STATS = 5   //system_stats_t (requires system handle)
} object_info_topic_t;

//info structures
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

//result struct for channel_recv_msg
typedef struct {
    size data_len;       //actual bytes of data received
    uint32 handle_count; //number of handles received
    uint32 sender_pid;   //PID of the process that sent this message (0 if kernel)
} channel_recv_result_t;

intptr syscall_dispatch(uintptr num, uintptr arg1, uintptr arg2, uintptr arg3,
                       uintptr arg4, uintptr arg5, uintptr arg6);

void syscall_init(void);

//internal syscall implementations
intptr sys_exit(intptr status);
intptr sys_getpid(void);
intptr sys_yield(void);
intptr sys_spawn(const char *path, int argc, char **argv);
intptr sys_wait(uintptr pid);
intptr sys_process_create(const char *name);
intptr sys_handle_grant(handle_t proc_h, handle_t local_h, handle_rights_t rights);
intptr sys_process_start(handle_t proc_h, uintptr entry, uintptr stack);
intptr sys_get_obj(handle_t parent, const char *path, handle_rights_t rights);
intptr sys_handle_close(handle_t h);
intptr sys_handle_dup(handle_t h, handle_rights_t new_rights);
intptr sys_ns_register(const char *path, handle_t h);
intptr sys_channel_create(int32 *ep0_out, int32 *ep1_out);
intptr sys_channel_send(handle_t ep, const void *data, size len);
intptr sys_channel_recv(handle_t ep, void *buf, size buflen);
intptr sys_channel_try_recv(handle_t ep, void *buf, size buflen);
intptr sys_channel_recv_msg(handle_t ep, void *data_buf, size data_len,
                           int32 *handles_buf, uint32 handles_len,
                           channel_recv_result_t *result_out);
intptr sys_channel_try_recv_msg(handle_t ep, void *data_buf, size data_len,
                               int32 *handles_buf, uint32 handles_len,
                               channel_recv_result_t *result_out);
intptr sys_vmo_create(size sz, uint32 flags, handle_rights_t rights);
intptr sys_vmo_read(handle_t h, void *buf, size len, size offset);
intptr sys_vmo_write(handle_t h, const void *buf, size len, size offset);
intptr sys_vmo_map(handle_t h, uintptr vaddr_hint, size offset, size len, uint32 flags);
intptr sys_vmo_unmap(uintptr vaddr, size len);
intptr sys_vmo_resize(handle_t vmo_h, size new_size);
intptr sys_stat(const char *path, stat_t *st);
intptr sys_fstat(handle_t h, stat_t *st);
intptr sys_readdir(handle_t h, dirent_t *entries, uint32 count, uint32 *index);
intptr sys_chdir(const char *path);
intptr sys_getcwd(char *buf, size bufsize);
intptr sys_mkdir(const char *path, uint32 mode);
intptr sys_remove(const char *path);
intptr sys_handle_read(handle_t h, void *buf, size len);
intptr sys_handle_write(handle_t h, const void *buf, size len);
intptr sys_handle_seek(handle_t h, size offset, int mode);
intptr sys_debug_write(const char *buf, size count);
intptr sys_get_ticks(void);
intptr sys_reboot(void);
intptr sys_shutdown(void);
intptr sys_object_get_info(handle_t h, uint32 topic, void *ptr, size len);

#endif
