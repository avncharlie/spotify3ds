#pragma once

#include <stdbool.h>

/* On-SD cache of decoded album art.
 *
 * Spotify's art URLs are content-addressed - the final path segment is a hash
 * of the image - so a cached entry can never go stale and needs no TTL. The
 * same album always yields the same URL and the same bytes.
 *
 * Threading invariant: only the worker thread writes. The render thread may
 * read. A future prefetcher must not break this.
 */

/* Measure SD throughput for a cache-sized entry. Phase 1 only: this decides
 * whether a cache is worth building at all, and whether a synchronous read is
 * cheap enough to do inside a frame. Emits TIMING lines; no-op unless timing
 * is enabled. */
void artcache_probe(void);
