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
 * `owner` and `art` may be NULL when the caller only wants the name; they come
 * back empty for entries cached before those fields existed. */
bool namecache_get(const char *uri, char *name, int namelen, char *owner,
                   int ownerlen, char *art, int artlen);

/* Record a name for `uri`, with optional owner and artwork url (either may be
 * NULL or empty). Best-effort: a failure just means the next launch
 * refetches. */
void namecache_put(const char *uri, const char *name, const char *owner,
                   const char *art);

/* Update the in-memory cache without writing the SD file. Bulk callers must
 * call namecache_flush() after publishing their results. */
void namecache_put_deferred(const char *uri, const char *name,
                            const char *owner, const char *art);
void namecache_flush(void);
