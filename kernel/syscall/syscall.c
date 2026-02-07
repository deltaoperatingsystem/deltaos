#include <syscall/syscall.h>
#include <arch/timer.h>

intptr syscall_dispatch(uintptr num, uintptr arg1, uintptr arg2, uintptr arg3,
                       uintptr arg4, uintptr arg5, uintptr arg6) {
    (void)arg4; (void)arg5; (void)arg6; //suppress unused parameter warnings
    switch (num) {
        case SYS_EXIT: return sys_exit((intptr)arg1);
        case SYS_GETPID: return sys_getpid();
        case SYS_YIELD: return sys_yield();
        case SYS_DEBUG_WRITE: return sys_debug_write((const char *)arg1, (size)arg2);
        case SYS_SPAWN: return sys_spawn((const char *)arg1, (int)arg2, (char **)arg3);
        case SYS_GET_OBJ: return sys_get_obj((handle_t)arg1, (const char *)arg2, (handle_rights_t)arg3);
        case SYS_HANDLE_READ: return sys_handle_read((handle_t)arg1, (void *)arg2, (size)arg3);
        case SYS_HANDLE_WRITE: return sys_handle_write((handle_t)arg1, (const void *)arg2, (size)arg3);
        case SYS_HANDLE_SEEK: return sys_handle_seek((handle_t)arg1, (size)arg2, (int)arg3);
        case SYS_HANDLE_CLOSE: return sys_handle_close((handle_t)arg1);
        case SYS_HANDLE_DUP: return sys_handle_dup((handle_t)arg1, (handle_rights_t)arg2);
        case SYS_CHANNEL_CREATE: return sys_channel_create((int32 *)arg1, (int32 *)arg2);
        case SYS_CHANNEL_SEND: return sys_channel_send((handle_t)arg1, (const void *)arg2, (size)arg3);
        case SYS_CHANNEL_RECV: return sys_channel_recv((handle_t)arg1, (void *)arg2, (size)arg3);
        case SYS_CHANNEL_TRY_RECV: return sys_channel_try_recv((handle_t)arg1, (void *)arg2, (size)arg3);
        case SYS_VMO_CREATE: return sys_vmo_create((size)arg1, (uint32)arg2, (handle_rights_t)arg3);
        case SYS_VMO_READ: return sys_vmo_read((handle_t)arg1, (void *)arg2, (size)arg3, (size)arg4);
        case SYS_VMO_WRITE: return sys_vmo_write((handle_t)arg1, (const void *)arg2, (size)arg3, (size)arg4);
        case SYS_CHANNEL_RECV_MSG: return sys_channel_recv_msg((handle_t)arg1, (void *)arg2, (size)arg3,
                                                                (int32 *)arg4, (uint32)arg5,
                                                                (channel_recv_result_t *)arg6);
        case SYS_CHANNEL_TRY_RECV_MSG: return sys_channel_try_recv_msg((handle_t)arg1, (void *)arg2, (size)arg3,
                                                                (int32 *)arg4, (uint32)arg5,
                                                                (channel_recv_result_t *)arg6);
        case SYS_VMO_MAP: return sys_vmo_map((handle_t)arg1, (uintptr)arg2, (size)arg3, (size)arg4, (uint32)arg5);
        case SYS_VMO_UNMAP: return sys_vmo_unmap((uintptr)arg1, (size)arg2);
        case SYS_NS_REGISTER: return sys_ns_register((const char *)arg1, (handle_t)arg2);
        case SYS_STAT: return sys_stat((const char *)arg1, (stat_t *)arg2);
        case SYS_WAIT: return sys_wait((uintptr)arg1);
        
        case SYS_PROCESS_CREATE: return sys_process_create((const char *)arg1);
        case SYS_HANDLE_GRANT: return sys_handle_grant((handle_t)arg1, (handle_t)arg2, (handle_rights_t)arg3);
        case SYS_PROCESS_START: return sys_process_start((handle_t)arg1, arg2, arg3);
        
        case SYS_VMO_RESIZE: return sys_vmo_resize((handle_t)arg1, (size)arg2);
        case SYS_READDIR: return sys_readdir((handle_t)arg1, (dirent_t *)arg2, (uint32)arg3, (uint32 *)arg4);
        case SYS_CHDIR: return sys_chdir((const char *)arg1);
        case SYS_GETCWD: return sys_getcwd((char *)arg1, (size)arg2);
        case SYS_GET_TICKS: return sys_get_ticks();
        case SYS_MKDIR: return sys_mkdir((const char *)arg1, (uint32)arg2);
        case SYS_REMOVE: return sys_remove((const char *)arg1);
        case SYS_FSTAT: return sys_fstat((handle_t)arg1, (stat_t *)arg2);
        case SYS_REBOOT: return sys_reboot();
        case SYS_SHUTDOWN: return sys_shutdown();
        case SYS_OBJECT_GET_INFO: return sys_object_get_info((handle_t)arg1, (uint32)arg2, (void *)arg3, (size)arg4);
        
        default: return -1;
    }
}
