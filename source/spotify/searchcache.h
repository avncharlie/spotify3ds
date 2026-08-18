#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "searchindex.h"

/* SD-backed store for packed search indexes.
 *
 * One file per playlist under sdmc:/spotify/searchidx, so a lookup opens
 * exactly one file and never walks a directory - artcache records that opening
 * every entry at startup cost close to a minute on real hardware.
 *
 * Nothing here is load-bearing. A missing, truncated, hand-edited or deleted
 * entry reads as a miss, and a miss just means the collection is walked the way
 * it always was. Deleting any or all of these files at any time is safe.
 *
 * Albums are not stored: they have no snapshot id to validate against, and at
 * one or two pages there is almost nothing to save.
 */

/* Entries are tens of KB and there are at most a hundred, so the whole store
 * stays comfortably under ten megabytes - artcache alone is allowed five
 * gigabytes. A library of 65 playlists never reaches the limit. */
#define SEARCHCACHE_MAX_ENTRIES 100

/* Look up the index for `context_uri`. Returns NULL on a miss, on a corrupt
 * entry (which is deleted), or for anything that is not a playlist. */
searchindex *searchcache_load(const char *context_uri);

/* Persist `blob`, replacing any existing entry. Takes ownership of nothing;
 * the caller still frees `blob`. Best-effort: failures are logged once and
 * leave the store as it was. Slow - call after publishing to the UI. */
void searchcache_store(const char *context_uri, const unsigned char *blob,
                       size_t len);

/* Drop one entry, e.g. after its snapshot no longer matches. */
void searchcache_evict(const char *context_uri);

/* True when an entry for this collection is present on the card. Used to
 * notice that a stored index has gone missing - deleted by hand, or lost with
 * the card - while a copy of it is still held in memory. */
bool searchcache_has(const char *context_uri);
