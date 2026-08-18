#pragma once

#include <stdbool.h>

#include "recents.h"

#define TRACK_PAGE_MAX 50

typedef enum {
	TRACK_ITEM_TRACK = 0,
	TRACK_ITEM_EPISODE,
	TRACK_ITEM_UNAVAILABLE,
} track_item_kind;

typedef struct {
	char name[128];
	char artist[128];
	char album[128];
	char uri[128];
	char art_url[256];
	long duration_ms;
	int  source_index; /* absolute position in the album/playlist context */
	unsigned char playable;
	unsigned char is_local;
	unsigned char explicit_content;
	unsigned char kind;
} track_item;

typedef struct {
	collection_item collection;
	track_item      items[TRACK_PAGE_MAX];
	int             offset;
	int             count;
	int             total;
} track_page;

/* Fetch one normalized page. Ordering is deliberately never persisted here:
 * album and playlist edits should appear whenever a page is opened. Blocking;
 * worker thread only. Artwork bytes are cached separately by artcache.
 *
 * Search is the one exception, and only behind a check: searchindex keeps the
 * searchable text of a whole collection, and serves it only after Spotify's
 * snapshot_id confirms the playlist has not changed. Browsing never reads it.
 * Even a stale index cannot play the wrong track - playback resolves by uri,
 * not by position - so ordering there affects at most the tie-break between
 * equally ranked matches. */
player_result tracks_fetch_page(const collection_item *collection, int offset,
                                track_page *out, char *err, int errlen);
