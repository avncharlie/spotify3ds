#pragma once

#include <stdbool.h>

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

/* Convert the retained worst-first heap into display order. */
void track_search_finalize(track_search_results *results);

/* Copy one local result page into the ordinary Tracks screen model. */
void track_search_build_page(const track_search_results *results,
                             const collection_item *collection, int offset,
                             track_page *out);
