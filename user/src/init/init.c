#include <system.h>
#include <io.h>

int main(int argc, char *argv[]) {
    puts("[init] DeltaOS starting...\n");
    
    printf("[init] argc = %d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("[init] argv[%d] = %s\n", i, argv[i]);
    }
    
    printf("[init] PID = %d\n", (int)getpid());
    
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