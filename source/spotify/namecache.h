#pragma once

#include <stdbool.h>

/* SD-backed uri -> display name cache.
 *
 * recently-played reports the *uri* of the context a track was played from but
 * never its name, so every distinct playlist in the history costs a separate
 * GET /v1/playlists/{id}. That is the difference between one request and half a
 * dozen on a cold start, which is worth caching.
 *
 * Unlike album art - whose URLs are content-addressed and so can never go
 * stale - a playlist can be renamed under a stable uri, so entries carry a
 * timestamp and expire. The window is deliberately long: a rename is rare and
 * the cost of being wrong is a stale label until the next refresh.
 */

/* Entries older than this are ignored and refetched. */
#define NAMECACHE_TTL_DAYS 14

/* Look up `uri`. Returns false on a miss or an expired entry.
 *
 * `owner` may be NULL when the caller only wants the name; it comes back empty
 * for entries cached before the owner was known. */
bool namecache_get(const char *uri, char *name, int namelen, char *owner,
                   int ownerlen);

/* Record a name (and owner, which may be NULL or empty) for `uri`.
 * Best-effort: a failure just means the next launch refetches. */
void namecache_put(const char *uri, const char *name, const char *owner);
