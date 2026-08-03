#pragma once

#include <stdbool.h>

/* Bring up AC (wifi) + SOC (sockets). Safe to call more than once.
 * Returns true on success; err (if non-NULL) gets a short reason on failure. */
bool net_init(char *err, int errlen);

void net_exit(void);

/* Resolve a hostname to a dotted-quad string. Returns false on failure. */
bool net_resolve(const char *host, char *out_ip, int outlen);

/* Blocking TCP connect. Returns a socket fd, or -1. */
int net_tcp_connect(const char *host, int port, char *err, int errlen);

void net_close(int fd);
