#include <lib/path.h>
#include <lib/string.h>

int path_normalize(char *path) {
    if (!path || path[0] == '\0') return 0;
    
    //work on a copy since strtok modifies in place
    char temp[512];
    strncpy(temp, path, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    
    char *components[64];
    int count = 0;
    bool absolute = (temp[0] == '/');
    bool is_namespace = (temp[0] == '$');
    
    //for namespace paths like $files/system we need to preserve the namespace
    char *ns_part = NULL;
    char *tokstart = temp;
    
    if (is_namespace) {
        //find the first slash to separate namespace from path
        char *slash = strchr(temp, '/');
        if (slash) {
            *slash = '\0';
            ns_part = temp;  //e.x. "$files"
            tokstart = slash + 1;
        } else {
            //just a namespace, no path
            return 0;
        }
    } else if (absolute) {
        tokstart = temp + 1;  //skip leading /
    }
    
    //tokenize the path part without strtok (non-reentrant)
    char *p = tokstart;
    while (*p && count < 64) {
        //skip consecutive slashes
        while (*p == '/') p++;
        if (!*p) break;
        //find end of token
        char *end = p;
        while (*end && *end != '/') end++;
        //null-terminate in-place; remember whether there was a separator
        char sep = *end;
        *end = '\0';
        size tlen = (size)(end - p);
        if (tlen == 1 && p[0] == '.') {
            //ignore .
        } else if (tlen == 2 && p[0] == '.' && p[1] == '.') {
            if (count > 0 && strcmp(components[count - 1], "..") != 0) {
                count--;
            } else if (!absolute && !is_namespace) {
                components[count++] = p;
            }
        } else if (tlen > 0) {
            components[count++] = p;
        }
        p = sep ? end + 1 : end;
    }
    
    //reconstruct into original path buffer
    char *write = path;
    
    if (is_namespace && ns_part) {
        size ns_len = strlen(ns_part);
        memcpy(write, ns_part, ns_len);
        write += ns_len;
    }
    
    if (absolute || is_namespace) {
        *write++ = '/';
    }
    
    for (int i = 0; i < count; i++) {
        size len = strlen(components[i]);
        memcpy(write, components[i], len);
        write += len;
        if (i < count - 1) {
            *write++ = '/';
        }
    }
    *write = '\0';
    
    //special case: absolute path with no components = "/"
    if (absolute && count == 0) {
        path[0] = '/';
        path[1] = '\0';
    }
    
    return 0;
}
