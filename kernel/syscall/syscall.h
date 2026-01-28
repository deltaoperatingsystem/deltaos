#ifndef SYSCALL_SYSCALL_H
#define SYSCALL_SYSCALL_H

#include <arch/types.h>

//syscall numbers
#define SYS_EXIT            0
#define SYS_GETPID          1
#define SYS_YIELD           2
#define SYS_DEBUG_WRITE     3
#define SYS_SPAWN           4   //spawns a new process
#define SYS_GET_OBJ         5   //get object from namespace
#define SYS_HANDLE_READ     6   //read from handle
#define SYS_HANDLE_WRITE    7   //write to handle
#define SYS_HANDLE_SEEK     8   //seek to an offset in a handle

//capability syscalls
#define SYS_HANDLE_CLOSE    32
#define SYS_HANDLE_DUP      33
#define SYS_CHANNEL_CREATE  34
#define SYS_CHANNEL_SEND    35
#define SYS_CHANNEL_RECV    36
#define SYS_VMO_CREATE      37
#define SYS_VMO_READ        38
#define SYS_VMO_WRITE       39
#define SYS_VMO_MAP         40  //map VMO into address space
#define SYS_VMO_UNMAP       41  //unmap from address space
#define SYS_NS_REGISTER     42  //register handle in namespace
#define SYS_STAT            43  //get file status by path
#define SYS_CHANNEL_TRY_RECV 44  //non-blocking channel receive
#define SYS_CHANNEL_RECV_MSG 45  //receive with handles
#define SYS_CHANNEL_TRY_RECV_MSG 46 //non-blocking recv_msg
#define SYS_WAIT            47  //wait for process to exit

//capability-based process creation (Zircon-style)
#define SYS_PROCESS_CREATE  50  //create suspended process, returns handle
#define SYS_HANDLE_GRANT    51  //inject handle into child process
#define SYS_PROCESS_START   52  //start initial thread in process
#define SYS_VMO_RESIZE      53  //resize a VMO
#define SYS_READDIR         54  //read directory entries
#define SYS_CHDIR           55  //change current working directory
#define SYS_GETCWD          56  //get current working directory
#define SYS_GET_TICKS       57  //get timer ticks since boot
#define SYS_MKDIR           58  //create directory
#define SYS_REMOVE          59  //remove file or directory
#define SYS_FSTAT           60  //get file status by handle

#define SYS_MAX             64

//result struct for channel_recv_msg
typedef struct {
    size data_len;       //actual bytes of data received
    uint32 handle_count; //number of handles received
    uint32 sender_pid;   //PID of the process that sent this message (0 if kernel)
} channel_recv_result_t;

int64 syscall_dispatch(uint64 num, uint64 arg1, uint64 arg2, uint64 arg3,
                       uint64 arg4, uint64 arg5, uint64 arg6);

void syscall_init(void);

#endif
