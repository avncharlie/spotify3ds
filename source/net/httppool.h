#pragma once

#include "tls.h"

/* Keep-alive connection cache.
 *
 * A TLS handshake to Spotify costs 700-1500ms, and the app previously paid one
 * on every single request: three per track skip, plus one every poll. Holding
 * connections open across requests removes almost all of that.
 *
 * Small and fixed-size on purpose. Requests are issued serially from the worker
 * thread, and the least recently used connection is evicted when API, auth,
 * lyrics, and image traffic span more hosts than the pool can retain.
 */

/* Take a connection to host:port, reusing a cached one when available.
 * Returns NULL on failure. */
tls_conn *pool_take(const char *host, int port, bool *reused, char *err,
                    int errlen);

/* Give a connection back for reuse. Pass keep=false when the exchange left the
 * stream in an unknown state (error, or the peer said Connection: close), in
 * which case it is closed rather than cached. */
void pool_give(const char *host, int port, tls_conn *c, bool keep);

/* Close every cached connection. */
void pool_clear(void);
