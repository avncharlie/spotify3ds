#pragma once

#include <3ds.h>
#include <stdbool.h>

/* On-SD cache of decoded album art.
 *
 * Spotify's art URLs are content-addressed - the final path segment is a hash
 * of the image - so a cached entry can never go stale and needs no TTL. The
 * same album always yields the same URL and the same bytes.
 *
 * Entries hold *pre-tiled* texture data, so a hit skips the network fetch, the
 * JPEG decode, the Morton tiling and the accent extraction: what remains is one
 * read plus a handful of memcpys.
 *
 * Measured on a New 2DS XL:
 *   cold   fetch 1241ms + decode 87ms = 1328ms
 *   warm   read 52ms
 *   store  111ms (12ms tiling + CRC, 75ms file IO)
 * so a repeat cover appears inside a frame rather than after a second and a
 * third. Stores still cost twice a read, which is why artcache_store runs only
 * after the art has already been handed to the UI.
 *
 * Threading invariant: only the worker thread writes. The render thread may
 * read. A future prefetcher must not break this.
 */

/* Bump when the payload layout, ART_TEX_SIZE, the JPEG scale factor or the
 * pixel byte order changes: entries with a different version are discarded. */
#define ARTCACHE_VERSION 3

/* Create the cache root and drop any .tmp files orphaned by a power loss.
 * Cheap: does not scan entries. */
void artcache_init(void);

/* Look up art for `url`.
 *
 * On a hit, *out_tiled is a linearAlloc'd, fully populated texture buffer ready
 * for C3D_TexLoadImage - the caller owns it and must linearFree it - and the
 * accent colour and source dimensions are restored. Returns false on a miss, a
 * corrupt entry (which is deleted), or any I/O error. */
bool artcache_load(const char *url, u8 **out_tiled, int *out_w, int *out_h,
                   int *out_dim, u8 *accent_r, u8 *accent_g, u8 *accent_b,
                   unsigned *read_ms);

/* Store decoded RGBA under `url`. Best-effort: failures are logged once and
 * never propagate, so a full or read-only card degrades to today's behaviour.
 * Writes are slow (~140ms), so call this *after* publishing to the UI. */
void artcache_store(const char *url, const u8 *rgba, int w, int h, u8 accent_r,
                    u8 accent_g, u8 accent_b);

/* Phase 1 measurement probe. Emits TIMING lines; no-op unless timing is on. */
void artcache_probe(void);
