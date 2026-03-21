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

// get user status struct
struct getusr_stat {
    struct passwd* pwd; // pointer to result (if succeeded)
    enum {
        G_OK, // succeeded
        G_EINTR, // internal error
        G_ENUSR // user not exist
    } status;
};

// get user
// params:
//      username - string containing username to search
// returns:
//      a pointer to a getusr_stat struct
//      this should be free'd with free_get_user_stat
//      if we failed to allocate the result struct, we'll return NULL
struct getusr_stat* get_user(const char* username);

// free get user status
// params:
//      stat - pointer to result from `get_user` function
void free_get_user_stat(struct getusr_stat* stat);

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
