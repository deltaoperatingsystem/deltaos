#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include "passwd.h"

// this returns a malloc'd string, make sure to free it!!!
char* serialize_passwd(struct passwd* pwd) {
    size_t sz = strlen(pwd->username) + strlen(pwd->pwd_hash) + 2; // username + delim (,) + hex hash + null ending
    char* str = malloc(sz);
    if (str == NULL) {
        return NULL;
    }
    
    if (snprintf(str, sz, "%s,%s", pwd->username, pwd->pwd_hash) != (sz - 1)) {
        free(str);
        return NULL;
    }
    return str;
}

// returns a malloc'd struct, free it!
struct passwd* deserialize_passwd(char* str) {
    size_t len = strlen(str);
    if (len > 0 && str[len - 1] == '\n') {
        str[--len] = '\0';
    }
    
    char* delim = strchr(str, ',');
    if (delim == NULL) {
        errno = EINVAL;
        return NULL;
    }
    
    *delim = '\0';
    char* pwd_hash = delim + 1;
    
    size_t username_len = strlen(str);
    size_t hash_len = strlen(pwd_hash);
    if (username_len > 256 || hash_len != 64) {
        errno = EINVAL;
        return NULL;
    }
    
    struct passwd* pwd = calloc(1, sizeof(*pwd));
    if (pwd == NULL) {
        return NULL;
    }
    
    memcpy(pwd->username, str, username_len + 1);
    memcpy(pwd->pwd_hash, pwd_hash, hash_len + 1);
    
    return pwd;
}
