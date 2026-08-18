#pragma once

#include <citro2d.h>
#include <stdbool.h>

#include "../spotify/searchhistory.h"
#include "touch.h"

/* The recent-searches panel, drawn above a finished bottom-screen view.
 *
 * Both headers put the same green search disc in the same place, so the panel
 * and its hold indicator are written once here rather than twice in the two
 * screens. Like volume_overlay this owns no state - everything arrives through
 * the args - but unlike it, this one is touchable and so registers hit rects.
 */

enum {
	SEARCH_POP_DISMISS = 1900, /* anywhere outside the panel */
	SEARCH_POP_CLEAR,
	SEARCH_POP_TYPE,
	SEARCH_POP_ROW0 = 1910, /* .. + SEARCHHISTORY_SHOWN - 1 */
};

typedef struct {
	C2D_TextBuf    buf;
	touch_builder *tb;
	const char    *queries[SEARCHHISTORY_SHOWN];
	int            count; /* how many of `queries` are set */
	int            pressed_id;
} search_popover_args;

/* Fill the ring around the search disc as it is held, so a long press has
 * some answer before it completes. `progress` is 0..1 of the way to
 * TOUCH_LONG_PRESS_MS; nothing is drawn early on, since a ring flashing up
 * under every ordinary tap reads as a glitch. */
void search_popover_draw_ring(float cx, float cy, float progress);

/* Hit rects must be registered before the screen underneath registers its
 * own: touch_hit takes the first match, so whoever goes in first wins. The
 * drawing has to happen after, to land on top. Hence the two calls. */
void search_popover_add_hits(const search_popover_args *a);
void search_popover_draw(const search_popover_args *a);
