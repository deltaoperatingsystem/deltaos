#include <system.h>
#include <sys/syscall.h>

int spawn(char *path, int argc, char **argv) {
    return spawn_ctx(path, argc, argv, NULL, 0);
}

int spawn_ctx(char *path, int argc, char **argv, const context_spawn_entry_t *entries, size entry_count) {
    return __syscall5(SYS_SPAWN_CTX, (long)path, argc, (long)argv, (long)entries, (long)entry_count);
}
