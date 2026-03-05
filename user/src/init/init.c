#include <system.h>
#include <io.h>

int main(int argc, char *argv[]) {
    puts("[init] DeltaOS starting...\n");
    (void)argc;
    (void)argv;
    
    //spawn shell
    puts("[init] starting shell...\n");
    int shell_pid = spawn("$files/system/binaries/shell", 0, NULL);
    if (shell_pid < 0) {
        printf("[init] failed to start shell (error %d)\n", shell_pid);
        return 1;
    }
    
    //wait for shell to exit
    wait(shell_pid);
    
    puts("[init] shell exited, system halting.\n");
    return 0;
}
