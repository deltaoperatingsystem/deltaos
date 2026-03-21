#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <fs.h>
#include <system.h>

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
    
    handle_t passwd = get_obj(INVALID_HANDLE, "$files/conf/passwd", RIGHT_WRITE | RIGHT_READ);
    if (passwd == INVALID_HANDLE) {
        return -1;
    }
    
    if (handle_seek(passwd, 0, HANDLE_SEEK_END) < 0) {
        handle_close(passwd);
        return -1;
    }
    
    char* pwd_str = serialize_passwd(&pwd);
    if (pwd_str == NULL) {
        handle_close(passwd);
        return -1;
    }
    
    size_t pwdstr_len = strlen(pwd_str) + 1;
    char pwdstr_nl[pwdstr_len];
    if (snprintf(pwdstr_nl, pwdstr_len, "%s\n", pwd_str) != pwdstr_len) {
        handle_close(passwd);
        return -1;
    }
    
    if (handle_write(passwd, pwdstr_nl, pwdstr_len) != pwdstr_len) {
        handle_close(passwd);
        return -1;
    }
    
    free(pwd_str);
    handle_close(passwd);
    
    return 0;
}

// the struct returnd will be malloc'd, free it when your done!
struct getusr_stat* get_user(const char* username) {
    struct getusr_stat* result = calloc(1, sizeof(*result));
    if (result == NULL) {
        return NULL;
    }
    
    handle_t hdl = get_obj(INVALID_HANDLE, "$files/conf/passwd", RIGHT_READ);
    if (hdl == INVALID_HANDLE) {
        result->status = G_EINTR;
        return result;
    }
    
    const size_t line_sz = 256 + 1 + 64 + 2; // usrname + delim + pwdhash + endings
    
    char line[line_sz];
    while (handle_getstr(line, line_sz, hdl) != NULL) {
        struct passwd* pwd = deserialize_passwd(line);
        if (pwd == NULL) {
            continue;
        }
        if (strcmp(pwd->username, username) == 0) {
            handle_close(hdl);
            result->status = G_OK;
            result->pwd = pwd;
            return result;
        } else {
            free(pwd);
        }
    }
    
    handle_close(hdl);
    result->status = G_ENUSR;
    return result;
}

void free_get_user_stat(struct getusr_stat* stat) {
    if (stat == NULL) return;
    
    if (stat->pwd != NULL) {
        free(stat->pwd);
    }
    
    free(stat);
}

enum verif_stat verify_user(const char* username, const char* pt_pwd) {
    enum verif_stat code;
    
    struct getusr_stat* passwd = get_user(username);
    if (passwd == NULL) {
        return V_EINTR;
    }
    
    if (passwd->status == G_ENUSR) {
        free_get_user_stat(passwd);
        return V_ENUSR;
    } 
    
    if (passwd->status == G_EINTR) {
        free_get_user_stat(passwd);
        return V_EINTR;
    }
    
    uint8_t shabuf[32] = {0};
    char shahex[65] = {0};
    sha256(pt_pwd, strlen(pt_pwd), shabuf);
    sha256_to_hex(shabuf, shahex);
    
    if (ct_memcmp(passwd->pwd->pwd_hash, shahex, 64)) {
        code = V_VALID;
    } else {
        code = V_EWPWD;
    }
    
    free(passwd);
    free_get_user_stat(passwd);
    return code;
}
