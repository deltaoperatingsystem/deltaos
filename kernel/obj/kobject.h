#ifndef OBJ_KOBJECT_H
#define OBJ_KOBJECT_H

//base types (from object.h)
// OBJECT_NONE    0
// OBJECT_FILE    1
// OBJECT_DIR     2
// OBJECT_DEVICE  3
// OBJECT_PIPE    4

//kernel object types
#define OBJECT_PROCESS  10   //process object
#define OBJECT_THREAD   11   //thread object
#define OBJECT_CHANNEL  12   //IPC channel endpoint
#define OBJECT_VMO      13   //virtual memory object
#define OBJECT_PORT     14   //async notification port
#define OBJECT_EVENT    15   //event object (signalable)
#define OBJECT_JOB      16   //job (process group)
#define OBJECT_NS_DIR   17   //namespace directory
#define OBJECT_INFO     18   //kernel info object
#define OBJECT_SOCKET   19   //network socket (TCP connection)

//type name helper
static inline const char *object_type_name(uint32 type) {
    switch (type) {
        case 0:  return "none";
        case 1:  return "file";
        case 2:  return "dir";
        case 3:  return "device";
        case 4:  return "pipe";
        case 10: return "process";
        case 11: return "thread";
        case 12: return "channel";
        case 13: return "vmo";
        case 14: return "port";
        case 15: return "event";
        case 16: return "job";
        case 17: return "ns_dir";
        case 18: return "info";
        case 19: return "socket";
        default: return "unknown";
    }
}


#endif
