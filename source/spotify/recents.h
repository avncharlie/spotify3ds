#pragma once

#include <stdbool.h>

#include "player.h"

/* Collections the user can jump back into: recently played, and their own
 * playlist library.
 *
 * Both feed the same row shape - art, name, subtitle - so they share a type.
 * The shelf on the player screen shows the first four recents; the Library
 * screen shows the rest plus every playlist.
 */

/* Recently played, after dedupe. Spotify returns one entry per *track*, and a
 * user listening through one album collapses to a single collection: 50 raw
 * items measured down to 4 distinct ones. 16 is well clear of any realistic
 * result while staying small enough to copy under lock. */
#define RECENTS_MAX 16

/* One page of /me/playlists. The API returns at most 50 per page and paging
 * further would cost a round trip per page for rows far below the fold. */
#define PLAYLISTS_MAX 50

/* Tiles on the player screen. The Library screen shows everything past these. */
#define SHELF_TILES 4

typedef enum {
	COLLECTION_ALBUM = 0,
	COLLECTION_PLAYLIST,
} collection_kind;

typedef struct {
	char            name[128];     /* album or playlist name */
	char            subtitle[128]; /* "Album - Artist" / "Playlist - Owner" */
	char            art_url[256];  /* empty when the collection has no image */
	char            context_uri[128]; /* what to play when tapped */
	collection_kind kind;
} collection_item;

typedef struct {
	collection_item items[RECENTS_MAX];
	int             count;
} recent_list;

typedef struct {
	collection_item items[PLAYLISTS_MAX];
	int             count;
	int             total; /* what Spotify reports, which may exceed count */
} playlist_list;

/* Kept for the existing call sites, which predate playlists sharing the type. */
typedef collection_item recent_item;

/* GET /v1/me/player/recently-played?limit=50. Blocking; worker thread only.
 *
 * Deduped by what tapping would play: the playlist context when there is one,
 * otherwise the album. Playlist names cost a second request each and are
 * resolved through the SD-backed name cache, so a repeat launch is free. */
player_result recents_fetch(recent_list *out, char *err, int errlen);

/* GET /v1/me/playlists?limit=50. Blocking; worker thread only.
 *
 * Uses `fields=` to drop the 60% of the response we never read - 52KB down to
 * 21KB measured - which this endpoint, unlike recently-played, honours. */
player_result playlists_fetch(playlist_list *out, char *err, int errlen);
