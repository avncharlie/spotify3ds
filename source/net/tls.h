#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct tls_conn tls_conn;

/* Connect to host:port and complete a TLS 1.2 handshake with full certificate
 * verification against the embedded roots.
 * Returns NULL on failure and fills err with a diagnosable reason. */
tls_conn *tls_connect(const char *host, int port, char *err, int errlen);

/* Negotiated ciphersuite name, e.g. "TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256". */
const char *tls_ciphersuite(const tls_conn *c);

/* TLS protocol version, e.g. "TLSv1.2". */
const char *tls_version(const tls_conn *c);

/* Write all len bytes. Returns true on success. */
bool tls_write(tls_conn *c, const void *buf, size_t len);

/* Read up to len bytes. Returns bytes read, 0 on clean close, <0 on error. */
int tls_read(tls_conn *c, void *buf, size_t len);

void tls_close(tls_conn *c);
