#ifndef AUTH_H
#define AUTH_H
#include "config.h"

/* out = "<16 hex salt>$<64 hex hash>" (salted, iterated SHA-256) */
void auth_hash_pin(const char *pin, char out[HASH_LEN]);
int  auth_verify_pin(const char *pin, const char *stored);
#endif
