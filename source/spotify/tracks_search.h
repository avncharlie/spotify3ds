#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "tracks.h"

#define TRACK_SEARCH_RESULTS_MAX 500
#define TRACK_SEARCH_QUERY_MAX   63

typedef struct {
	track_item    item;
	unsigned char rank;
} track_search_hit;

typedef struct {
	track_search_hit *hits;
	int               count;
	int               matched_total;
	int               source_total;
	bool              truncated;
} track_search_results;

void track_search_results_init(track_search_results *results);
void track_search_results_free(track_search_results *results);
void track_search_results_move(track_search_results *dst,
                               track_search_results *src);

/* Duplicate the retained hits so a partial snapshot can be published while the
 * scan keeps filling the original heap. */
bool track_search_results_copy(track_search_results *dst,
                               const track_search_results *src);

/* Add matching tracks from one source page. `match_album` should be false for
 * album collections, where every row has the same album name. */
bool track_search_consider_page(track_search_results *results,
                                const track_page *page, const char *query,
                                bool match_album);

/* --- matching, length-bounded ------------------------------------------
 * A cached index stores text packed without terminators, so these take
 * explicit lengths. The NUL-terminated paths above are written on top of
 * them: both callers must rank identically, or a warm cache would quietly
 * disagree with a live scan. */

/* 3 exact, 2 prefix, 1 interior, 0 no match. Case-insensitive. */
int track_search_match_quality_n(const char *text, size_t textlen,
                                 const char *query, size_t querylen);

/* Rank one candidate from already-split fields: 0-2 name, 3-5 artist,
 * 6-8 album, -1 for no match at all. */
int track_search_rank_fields(const char *name, size_t namelen,
                             const char *artist, size_t artistlen,
                             const char *album, size_t albumlen,
                             const char *query, size_t querylen,
                             bool match_album);

/* Offer one already-ranked item to the retained set. Keeps the cap, the
 * tie-break and the `truncated` accounting in one place. */
bool track_search_consider_hit(track_search_results *results,
                               const track_item *item, int rank);

/* Convert the retained worst-first heap into display order. */
void track_search_finalize(track_search_results *results);

/* Copy one local result page into the ordinary Tracks screen model. */
void track_search_build_page(const track_search_results *results,
                             const collection_item *collection, int offset,
                             track_page *out);
