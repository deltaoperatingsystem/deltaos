#include <system.h>
#include <io.h>

int main(int argc, char *argv[]) {
    puts("[init] DeltaOS starting...\n");
    (void)argc;
    (void)argv;

    puts("[init] starting login...\n");
    int login_pid = spawn("$files/system/binaries/login", 0, NULL);
    if (login_pid < 0) {
        printf("[init] failed to start login (error %d)\n", login_pid);
        return 1;
    }

    wait(login_pid);

    puts("[init] login exited, system halting.\n");
    return 0;
}
