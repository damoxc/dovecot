#ifndef __COMMON_H
#define __COMMON_H

#include "lib.h"
#include "../auth/auth-interface.h"

typedef struct _Client Client;
typedef struct _AuthRequest AuthRequest;

extern int disable_plaintext_auth;
extern unsigned int max_logging_users;

void main_ref(void);
void main_unref(void);

void main_close_listen(void);

#endif
