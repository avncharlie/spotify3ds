#pragma once

#include <citro2d.h>
#include <stdbool.h>

#include "../spotify/player.h"
#include "touch.h"

/* Bottom screen, player view (1B in the mockup).
 *
 * Recently-played shelf across the top, five transport controls on one
 * baseline with no button boxes, and an inline scrubber.
 */

/* Control ids. Shelf tiles occupy BTN_SHELF0..BTN_SHELF0+3, and the fifth tile
 * is BTN_SHELF_ALL, which opens the full list. */
enum {
	BTN_PREV = 0,
	BTN_PLAY,
	BTN_NEXT,
	BTN_SHUFFLE,
	BTN_REPEAT,
	BTN_SCRUB,
	BTN_SHELF_ALL,
	BTN_SHELF0, /* .. BTN_SHELF0 + SHELF_TILES - 1 */
};

#define SHELF_TILES 4

typedef struct {
	C2D_TextBuf buf;
	touch_builder *tb;

	bool        playing;
	bool        shuffle;
	repeat_mode repeat;

	long progress_ms;
	long duration_ms;

	/* Which control the finger is currently down on, or -1. Used only to give
	 * a pressed appearance. */
	int  pressed_id;
	bool scrubbing;

	/* Shelf art, NULL where not yet loaded. */
	const C2D_Image *shelf[SHELF_TILES];
	bool             shelf_current[SHELF_TILES];
	unsigned         animation_ms;
} screen_player_args;

void screen_player_draw(const screen_player_args *a);

/* Scrubber geometry, shared with the input handler so a tap maps to the same
 * track the user sees. */
#define SCRUB_BAR_X 50.0f
#define SCRUB_BAR_W 220.0f
#define SCRUB_BAR_Y 208.0f
