#ifndef __USER_H
#define __USER_H

#include "passwd.h"

// create user
// params:
//      username - the username for the user to create
//      pwd - the plaintext password for the user to create, this will be hashed internally
// return codes:
//      -1 - an error occured
//       0 - success
int create_user(const char* username, const char* pwd);

#define GETUSR_INTERNAL_ERROR ((void*)-1)
#define GETUSR_NOT_EXIST ((void*)0)

// get user
// params:
//      username - string containing username to search
// return codes:
//      GETUSR_INTERNAL_ERROR - an internal error occured, for example failing to open passwd file
//      GETUSR_NOT_EXIST - user does not exist
//      struct passwd* - a `malloc`d pointer to a passwd struct, meaning success
struct passwd* get_user(const char* username);

enum verif_stat {
    V_EWPWD, // wrong password
    V_ENUSR, // user not exist
    V_EINTR, // internal error
    V_VALID // valid login
};

// verify user
// params:
//      username - the user to check
//      pwd - the plaintext password to verify
// return codes:
//      V_EWPWD - wrong password
//      V_ENUSR - user doesnt exist
//      V_EINTR - internal error occured
//      V_VALID - valid passwod (success)
enum verif_stat verify_user(const char* username, const char* pwd);

#endif
