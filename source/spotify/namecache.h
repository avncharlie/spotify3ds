#pragma once

#include <stdbool.h>
#include "playlist_meta.h"

/* SD-backed uri -> display name cache.
 *
 * recently-played reports the *uri* of the context a track was played from but
 * never its name, so every distinct playlist in the history costs a separate
 * GET /v1/playlists/{id}. That is the difference between one request and half a
 * dozen on a cold start, which is worth caching.
 *
 * Fields retain their source and timestamp independently. Web API fields live
 * for a fortnight; public oEmbed fallbacks live for one day. Generated images
 * have a separate expiry in the artwork cache.
 */

/* Entries older than this are ignored and refetched. */
#define NAMECACHE_TTL_DAYS 14
#define NAMECACHE_FALLBACK_TTL_DAYS 1

/* Partial, source-aware lookup. Only fresh fields are returned. */
bool namecache_lookup(const char *uri, playlist_meta *meta);
/* Merge fields independently; fresh Web API values outrank oEmbed. Deferred
 * write so callers can publish first and flush once after a batch. */
void namecache_store(const char *uri, const playlist_meta *meta);
time_t namecache_refresh_at(const char *uri);
void namecache_reset(void);

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
