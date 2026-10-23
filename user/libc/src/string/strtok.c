#include <string.h>

//reentrant tokenizer: all state lives in *saveptr
char *strtok_r(char *str, const char *delim, char **saveptr) {
    char *next = str ? str : *saveptr;
    if (!next) return NULL;

    //skip leading delimiters
    while (*next && strchr(delim, *next)) next++;
    if (!*next) {
        *saveptr = NULL;
        return NULL;
    }

    char *start = next;
    while (*next && !strchr(delim, *next)) next++;
    if (*next) {
        *next = '\0';
        *saveptr = next + 1;
    } else {
        *saveptr = NULL;
    }
    return start;
}

//legacy non-reentrant wrapper; prefer strtok_r
char *strtok(char *str, const char *delim) {
    static char *saved;
    return strtok_r(str, delim, &saved);
}