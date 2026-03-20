#ifndef __PASSWD_H
#define __PASSWD_H

#include <stdint.h>

// must NOT contain ','s or else you'll mess up deserialization
struct passwd {
    // the username of the user (256 chars max)
    char username[257];
    // the sha256 hash of the users password
    char pwd_hash[65];
};

// serialize a `passwd` struct
char* serialize_passwd(struct passwd* pwd);

// deserialize a serialized `passwd` struct
struct passwd* deserialize_passwd(char* str);

#endif
