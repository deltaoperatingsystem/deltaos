#ifndef __SYS_SYSNUMS_H
#define __SYS_SYSNUMS_H

//process management (SYS_INIT to 31)
#define SYS_EXIT            0
#define SYS_GETPID          1
#define SYS_YIELD           2
#define SYS_SPAWN           4   //spawns a new process
#define SYS_SPAWN_CTX       77  //spawn with per-child context overrides
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

//memory: vmos (53-56)
#define SYS_VMO_CREATE      37
#define SYS_VMO_READ        38
#define SYS_VMO_WRITE       39
#define SYS_VMO_MAP         40  //map vmo into address space
#define SYS_VMO_UNMAP       41  //unmap from address space
#define SYS_VMO_RESIZE      53  //resize a vmo

//filesystem (57-60)
#define SYS_STAT            43  //get file status by path
#define SYS_FSTAT           60  //get file status by handle
#define SYS_READDIR         54  //read directory entries
#define SYS_CHDIR           55  //change current working directory
#define SYS_GETCWD          56  //get current working directory
#define SYS_MOUNT           69  //mount a filesystem
#define SYS_CONTEXT_SET     72  //set a typed process context entry
#define SYS_CONTEXT_GET     73  //get a typed process context entry
#define SYS_CONTEXT_SET_HANDLE 74 //capture a handle-backed process context entry
#define SYS_CONTEXT_GET_HANDLE 75 //materialize a handle from a context entry
#define SYS_CONTEXT_REMOVE  76  //remove a process context entry
#define SYS_PROC_SEND_EVENT 78 //post an async event to another process
#define SYS_PROC_SET_EVENT_HANDLER 79 //install a userspace async event handler
#define SYS_PROC_MASK_EVENTS 80 //block selected events on the current thread
#define SYS_PROC_UNMASK_EVENTS 81 //unblock selected events on the current thread
#define SYS_PROC_GET_PENDING_EVENTS 82 //read current process pending event mask
#define SYS_PROC_EVENT_RETURN 83 //return from a userspace event handler
#define SYS_PROC_SET_CONSOLE_FOREGROUND 84 //set process receiving ctrl+c interrupts
#define SYS_MKNODE          58  //create fs node
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

//networking (64+)
#define SYS_PING            64  //send ICMP ping and wait for reply
#define SYS_DNS_RESOLVE     65
#define SYS_TCP_CONNECT     66  //connect to host:port, returns socket handle
#define SYS_TCP_LISTEN      67  //listen on port, returns listener handle
#define SYS_TCP_ACCEPT      68  //accept connection on listener, returns socket handle
#define SYS_DNS_RESOLVE_AAAA 71 //resolve hostname to IPv6 address (AAAA)

#define SYS_MAX             256

#endif
