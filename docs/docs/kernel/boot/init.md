# init

## Error codes
When init fails to spawn, one of 12 error codes will be present:

| code | meaning |
|------|---------|
|1|Failed to open init path. This usually means that either the cmdline path is incorrect, the file does not exist or there is no initrd (see [kernel/boot/initrd](/docs/kernel/boot/initrd.md))|
|2|Failed to allocate buffer for the init (OOM)|
|3|Failed to read the init binary into the buffer|
|4|Invalid init file (not an executable)|
|5|Failed to create the init process for it to run on|
|6|Failed to load the executable onto the process|
|7|Failed to open the interpreter (dynamic exe only)|
|8|Failed to allocate interpreter (dynamic exe only)|
|9|Invalid interpreter file (not an executable) (dynamic exe only)|
|10|Failed to load interpreter onto the process (dynamic exe only)|
|11|Failed to allocate process stack|
|12|Failed to create user thread for process entry|
