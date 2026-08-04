#pragma once

#include <citro2d.h>
#include <stdbool.h>

#include "../spotify/recents.h"
#include "touch.h"

/* Bottom screen, recently-played list (1C in the mockup).
 *
 * Opened from the shelf's ALL tile. 44px rows so a thumb can hit one on a
 * resistive panel, dragged vertically, tap to play.
 */

enum {
	LIST_BTN_BACK = 100, /* well clear of the player's ids */
	LIST_ROW0,           /* .. LIST_ROW0 + RECENTS_MAX - 1 */
};

#define LIST_HEADER_H 34.0f
#define LIST_ROW_H    44.0f

typedef struct {
	C2D_TextBuf        buf;
	touch_builder     *tb;
	const recent_list *items;
	const C2D_Image  **art; /* per item, NULL where not loaded */

	float scroll;     /* pixels scrolled down */
	int   pressed_id; /* control under the finger, or -1 */
} screen_list_args;

void screen_list_draw(const screen_list_args *a);

/* How far the list can scroll, given the item count. */
float screen_list_max_scroll(int count);
