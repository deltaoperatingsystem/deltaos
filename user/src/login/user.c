#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <fs.h>

#include "user.h"
#include "passwd.h"
#include "sha256.h"

int create_user(const char* username, const char* pt_pwd) {
    struct passwd pwd = {0};

    size_t usrname_len = strlen(username);
    
    if ((usrname_len > 256) || 
        (strchr(username, ',') != NULL) ||
        (strchr(username, '\n') != NULL)) {
        errno = EINVAL;
        return -1;
    }
    memcpy(pwd.username, username, usrname_len + 1);
    
    uint8_t shabuf[32];
    sha256(pt_pwd, strlen(pt_pwd), shabuf);
    sha256_to_hex(shabuf, pwd.pwd_hash);
    
    struct stat st;
    if (stat("/conf/passwd", &st) < 0) {
        if (stat("/conf", &st) < 0) {
            if (mkdir("/conf") < 0) {
                return -1;
            }
        }
        if (mkfile("/conf/passwd") < 0) {
            return -1;
        }
    }
    
    FILE* passwd = fopen("/conf/passwd", "aw");
    if (passwd == NULL) {
        return -1;
    }
    
    char* pwd_str = serialize_passwd(&pwd);
    if (pwd_str == NULL) {
        fclose(passwd);
        return -1;
    }
    
    if (fprintf(passwd, "%s\n", pwd_str) != (strlen(pwd_str) + 1)) {
        fclose(passwd);
        return -1;
    }
    
    free(pwd_str);
    fclose(passwd);
    
    return 0;
}

// the struct returnd will be malloc'd, free it when your done!
struct passwd* get_user(const char* username) {
    FILE* passwd = fopen("/conf/passwd", "r");
    if (passwd == NULL) {
        return NULL;
    }
    
    const size_t line_sz = 256 + 1 + 64 + 2; // usrname + delim + pwdhash + endings
    
    char line[line_sz];
    while (fgets(line, line_sz, passwd) != NULL) {
        struct passwd* pwd = deserialize_passwd(line);
        if (pwd == NULL) {
            continue;
        }
        if (strcmp(pwd->username, username) == 0) {
            fclose(passwd);
            return pwd;
        } else {
            free(pwd);
        }
    }
    
    fclose(passwd);
    return NULL;
}

enum verif_stat verify_user(const char* username, const char* pt_pwd) {
    enum verif_stat code;
    
    struct passwd* passwd = get_user(username);
    if (passwd == NULL) {
        return V_ENUSR; // its possible this was also an internal error, such as failing to open /conf/passwd
            // we should later make it so in that case, EINTR is actually returned, not ENUSR
    }
    
    uint8_t shabuf[32] = {0};
    char shahex[65] = {0};
    sha256(pt_pwd, strlen(pt_pwd), shabuf);
    sha256_to_hex(shabuf, shahex);
    
    if (ct_memcmp(passwd->pwd_hash, shahex, 64)) {
        code = V_VALID;
    } else {
        code = V_EWPWD;
    }
    
    free(passwd);
    return code;
}
