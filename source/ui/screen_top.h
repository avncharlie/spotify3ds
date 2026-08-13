#pragma once

#include <citro2d.h>
#include <stdbool.h>

#include "../spotify/art.h"

/* Top screen: cover, track metadata and the device line over a wash sampled
 * from the artwork.
 *
 * Two variants, per the mockup:
 *   1A  art on  - 200px cover left, text column at x=232
 *   2A  art off - one large left-aligned block owning the screen
 *
 * Everything the screen needs is passed in, so it draws from a snapshot and
 * never reaches into worker state.
 */
typedef struct {
	C2D_TextBuf      buf;
	const album_art *art;
	bool             art_hidden; /* user toggled art off (KEY_Y) */
	bool             have_state;
	bool             fatal;
	const char      *track;
	const char      *artist;
	const char      *album;
	const char      *device;
	const char      *status;
	const char      *hint;
	const char      *detail; /* raw error under a fatal status; may be NULL */
} screen_top_args;

void screen_top_draw(const screen_top_args *a);
