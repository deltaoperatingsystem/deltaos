#ifndef __PASSWD_H
#define __PASSWD_H

#include <stdint.h>

// must NOT contain ','s or else you'll mess up deserialization
struct passwd {
    char username[257];
    char pwd_hash[65];
};

char* serialize_passwd(struct passwd* pwd);
struct passwd* deserialize_passwd(char* str);

#endif
