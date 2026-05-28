#include <types.h>
#include <string.h>

bool streq(const char *a, const char *b) {
    while (*a && *b) if (*a++ != *b++) return false;
    return *a == *b;
}
