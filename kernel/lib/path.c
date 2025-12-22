#include <lib/path.h>
#include <lib/string.h>

int path_normalize(char *path) {
    if (!path) return -1;
    
    char *read = path;
    char *write = path;
    
    while (*read) {
        //check for . or .. components
        if (*read == '.') {
            //at start or after /
            if (read == path || *(read - 1) == '/') {
                //check for /. or /..
                if (read[1] == '/' || read[1] == '\0') {
                    return -1;  //single dot component not allowed
                }
                if (read[1] == '.' && (read[2] == '/' || read[2] == '\0')) {
                    return -1;  //double dot component not allowed
                }
            }
        }
        
        if (*read == '/') {
            //skip consecutive slashes
            while (read[1] == '/') {
                read++;
            }
        }
        
        *write++ = *read++;
    }
    *write = '\0';
    
    //strip trailing slash (but keep if path is just "/")
    size len = strlen(path);
    if (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
    }
    
    return 0;
}
