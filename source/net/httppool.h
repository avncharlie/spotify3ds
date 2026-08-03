#pragma once

#include "tls.h"

/* Keep-alive connection cache.
 *
 * A TLS handshake to Spotify costs 700-1500ms, and the app previously paid one
 * on every single request: three per track skip, plus one every poll. Holding
 * connections open across requests removes almost all of that.
 *
 * Small and fixed-size on purpose - the app only ever talks to three hosts
 * (api.spotify.com, accounts.spotify.com, i.scdn.co), and one live connection
 * each is enough because requests are issued serially from the worker thread.
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
