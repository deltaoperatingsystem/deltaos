#ifndef SYSCALL_SYSCALL_H
#define SYSCALL_SYSCALL_H

#include <arch/types.h>
#include <obj/handle.h>
#include <fs/fs.h>
#include <sys/sysnums.h>

//object info topics
typedef enum {
    OBJ_INFO_NONE = 0,
    OBJ_INFO_PROCESS_BASIC = 1, //process_info_basic_t
    OBJ_INFO_THREAD_STATS = 2,  //thread_stats_t
    OBJ_INFO_KMEM_STATS = 3,    //kmem_stats_t (requires system handle)
    OBJ_INFO_TIME_STATS = 4,    //time_stats_t (requires system handle)
    OBJ_INFO_SYSTEM_STATS = 5,  //system_stats_t (requires system handle)
    OBJ_INFO_BOOT_CMDLINE = 6,  //boot cmdline string (requires system handle)
    OBJ_INFO_BLOCK_DEVICE = 7,  //block_device_info_t (requires device handle)
    OBJ_INFO_VT_STATE = 8       //vt_info_t (requires vt device handle)
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
    uint32 sector_size;
    uint64 sector_count;
} block_device_info_t;

typedef struct {
    uint32 cols;
    uint32 rows;
    uint32 cursor_col;
    uint32 cursor_row;
} vt_info_t;

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

typedef enum {
    CONTEXT_VALUE_STRING = 1,
    CONTEXT_VALUE_I64 = 2,
    CONTEXT_VALUE_U64 = 3,
    CONTEXT_VALUE_BOOL = 4,
    CONTEXT_VALUE_OBJECT = 5
} context_value_type_t;

#define CONTEXT_FLAG_INHERIT   (1u << 0)
#define CONTEXT_FLAG_READONLY  (1u << 1)

typedef struct {
    const char *key;
    uint32 type;
    uint32 flags;
    size value_len;
    union {
        const void *ptr;
        int64 i64;
        uint64 u64;
        uint32 boolean;
        handle_t handle;
    } value;
} context_spawn_entry_t;

intptr syscall_dispatch(uintptr num, uintptr arg1, uintptr arg2, uintptr arg3,
                       uintptr arg4, uintptr arg5, uintptr arg6);

void syscall_init(void);

//internal syscall implementations
intptr sys_exit(intptr status);
intptr sys_getpid(void);
intptr sys_yield(void);
intptr sys_spawn(const char *path, int argc, char **argv);
intptr sys_spawn_ctx(const char *path, int argc, char **argv,
                     const context_spawn_entry_t *entries, size entry_count);
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
intptr sys_mount(handle_t source, const char *target, const char *fstype);
intptr sys_mknode(const char *path, uint32 type);
intptr sys_remove(const char *path);
intptr sys_handle_read(handle_t h, void *buf, size len);
intptr sys_handle_write(handle_t h, const void *buf, size len);
intptr sys_handle_seek(handle_t h, size offset, int mode);
intptr sys_debug_write(const char *buf, size count);
intptr sys_get_ticks(void);
intptr sys_reboot(void);
intptr sys_shutdown(void);
intptr sys_object_get_info(handle_t h, uint32 topic, void *ptr, size len);
intptr sys_ping(uint32 family, const void *dst_addr, uint32 addr_len, uint32 count);
intptr sys_dns_resolve(const char *hostname, uint32 *ip_out);
intptr sys_dns_resolve_aaaa(const char *hostname, uint8 *ipv6_out);
intptr sys_tcp_connect(uint32 family, const void *dst_addr, uint32 addr_len, uint16 port);
intptr sys_tcp_listen(uint32 family, const void *local_addr, uint32 addr_len, uint16 port);
intptr sys_tcp_accept(handle_t listen_h);
intptr sys_context_set(const char *key, uint32 type, const void *value_ptr, size value_len, uint32 flags);
intptr sys_context_get(const char *key, uint32 type, void *value_ptr, size value_len, uint32 *flags_out);
intptr sys_context_set_handle(const char *key, handle_t h, uint32 flags);
intptr sys_context_get_handle(const char *key, handle_t *out_h, uint32 *flags_out);
intptr sys_context_remove(const char *key);
intptr sys_proc_send_event(uintptr pid, uint32 event);
intptr sys_proc_set_event_handler(uint32 event, uintptr entry, uint32 flags);
intptr sys_proc_mask_events(uint64 mask);
intptr sys_proc_unmask_events(uint64 mask);
intptr sys_proc_get_pending_events(uint64 *out_mask);
intptr sys_proc_event_return(void);
intptr sys_proc_set_console_foreground(uintptr pid);

//helper for safe user-space copies
int copy_user_bytes(const void *user_ptr, void *kernel_buf, size len);
int copy_user_cstr(const char *user_str, char *kernel_buf, size kernel_len);
int copy_to_user_bytes(void *user_ptr, const void *kernel_buf, size len);

#endif
