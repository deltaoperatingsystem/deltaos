#ifndef LIB_ERRNO_H
#define LIB_ERRNO_H

#define EOK      0      //success
#define EPERM    1      //operation not permitted
#define ENOENT   2      //no such file or directory
#define ESRCH    3      //no such process
#define EINTR    4      //interrupted system call
#define EIO      5      //I/O error
#define ENXIO    6      //no such device or address
#define E2BIG    7      //argument list too long
#define ENOEXEC  8      //exec format error
#define EBADF    9      //bad file number
#define ECHILD   10     //no child processes
#define EAGAIN   11     //try again
#define ENOMEM   12     //out of memory
#define EACCES   13     //permission denied
#define EFAULT   14     //bad address
#define EBUSY    16     //device or resource busy
#define EEXIST   17     //file exists
#define EXDEV    18     //cross-device link
#define ENODEV   19     //no such device
#define ENOTDIR  20     //not a directory
#define EISDIR   21     //is a directory
#define EINVAL   22     //invalid argument
#define ENFILE   23     //file table overflow
#define EMFILE   24     //too many open files
#define ENOTTY   25     //not a typewriter
#define EFBIG    27     //file too large
#define ENOSPC   28     //no space left on device
#define ESPIPE   29     //illegal seek
#define EROFS    30     //read-only file system
#define EMLINK   31     //too many links
#define EPIPE    32     //broken pipe
#define EDOM     33     //math argument out of domain of func
#define ERANGE   34     //math result not representable
#define ENOSYS   38     //function not implemented
#define ENOTEMPTY 39    //directory not empty
#define ELOOP    40     //too many symbolic links encountered
#define ETIMEDOUT 110    //connection timed out
#define ENAMETOOLONG 78 //file name too long

#endif
