#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tracks.h"
#include "tracks_search.h"

/* Searchable corpus for one collection, packed small enough to keep in RAM and
 * to persist across launches.
 *
 * Searching a collection means paging the whole thing out of Spotify at 50
 * items per request, which is ~18s for a 1791-track playlist. That cost is
 * worth paying once, not once per query. This module holds the *text* a search
 * needs - name, artist, album, artwork and enough identity to play the track -
 * and nothing else, so the corpus is ~136 bytes a track rather than the 780 a
 * track_item occupies.
 *
 * Deliberately free of <3ds.h> so the format can be exercised by the host test
 * suite, which is where the round-trip and live-equivalence tests live.
 *
 * Ordering is not authority here: a cached index is only served once its
 * snapshot_id has been checked against Spotify, and playback resolves by uri
 * rather than by position, so a stale source_index can at worst disturb the
 * tie-break between equally ranked hits.
 */

#define SEARCHINDEX_VERSION 2

/* A playlist beyond this cannot be indexed: source_index is stored as u16 and
 * a wrap would silently corrupt the tie-break order. Spotify's own ceiling is
 * 10,000 items, so this is unreachable in practice. */
#define SEARCHINDEX_TRACKS_MAX 65534

/* Refuse to serialize anything larger. A 10,000-track playlist packs to about
 * 1.3MB, so this leaves headroom while keeping total disk bounded. */
#define SEARCHINDEX_BYTES_MAX (2u * 1024u * 1024u)

#define SEARCHINDEX_SNAPSHOT_MAX 63

typedef struct searchindex searchindex;
typedef struct searchindex_builder searchindex_builder;

/* --- building ----------------------------------------------------------
 * Pages are handed over as they arrive from the live scan; the builder keeps
 * only the packed bytes, never the pages themselves. */

searchindex_builder *searchindex_builder_new(const collection_item *collection,
                                             const char *snapshot_id,
                                             int item_total);
bool searchindex_builder_add_page(searchindex_builder *builder,
                                  const track_page *page);
void searchindex_builder_free(searchindex_builder *builder);

/* Hand over the finished blob. The builder keeps nothing; the caller owns
 * `out` and must free it. Fails if nothing was added or the cap was hit. */
bool searchindex_builder_finish(searchindex_builder *builder,
                                unsigned char **out, size_t *outlen);

/* --- reading -----------------------------------------------------------  */

/* Validate and adopt `blob` (magic, version, lengths, then CRC). Returns NULL
 * and frees nothing on rejection, so a corrupt file reads exactly like a
 * missing one. Takes ownership of `blob` on success. */
searchindex *searchindex_open(unsigned char *blob, size_t len);
void         searchindex_free(searchindex *index);

const char *searchindex_snapshot(const searchindex *index);

/* The bytes this index was opened from, for writing it back out. */
const unsigned char *searchindex_blob(const searchindex *index);
int         searchindex_count(const searchindex *index);
size_t      searchindex_bytes(const searchindex *index);

/* Rewrite a stored index's snapshot id in place, leaving it structurally
 * valid. Only the freshness check should reject the result - which is exactly
 * what happens when a playlist gains a track, and is otherwise impossible to
 * reproduce from a test without editing the playlist. */
bool searchindex_age_file_for_test(const char *path);

/* Rank the whole corpus against `query`, filling `results` exactly as a live
 * scan would. `collection` supplies what the records do not carry: the album
 * name for album collections, and the art url fallback. */
bool searchindex_search(const searchindex *index,
                        const collection_item *collection, const char *query,
                        track_search_results *results);
