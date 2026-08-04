#pragma once

#include <stdbool.h>

#include "player.h"

/* Recently played, for the shelf and the list view.
 *
 * Deliberately small: the shelf shows four tiles and the list about four and a
 * half rows, so there is no reason to hold more. The request is also kept
 * small on purpose - see recents_fetch.
 */
#define RECENTS_MAX 8

typedef struct {
	char name[128];     /* album or playlist name */
	char subtitle[128]; /* "Album - Artist" */
	char art_url[256];
	char context_uri[128]; /* what to play when tapped */
} recent_item;

typedef struct {
	recent_item items[RECENTS_MAX];
	int         count;
} recent_list;

/* GET /v1/me/player/recently-played. Blocking; worker thread only.
 *
 * Consecutive plays from the same album collapse into one entry - Spotify
 * returns one item per *track*, so a single album listened through would
 * otherwise fill the whole shelf with itself. */
player_result recents_fetch(recent_list *out, char *err, int errlen);
