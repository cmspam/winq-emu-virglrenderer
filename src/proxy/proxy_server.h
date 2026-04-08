/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef PROXY_SERVER_H
#define PROXY_SERVER_H

#include "proxy_common.h"

#ifndef _WIN32
#include <sys/types.h>
#endif

struct proxy_server {
#ifndef _WIN32
   pid_t pid;
#endif
   int client_fd;
#ifdef _WIN32
   /* opaque pointer to win32 server thread state */
   void *thread_data;
#endif
};

struct proxy_server *
proxy_server_create(void);

void
proxy_server_destroy(struct proxy_server *srv);

int
proxy_server_connect(struct proxy_server *srv);

#endif /* PROXY_SERVER_H */
