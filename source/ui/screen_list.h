#pragma once

#include <citro2d.h>
#include <stdbool.h>

#include "../spotify/recents.h"
#include "touch.h"

/* Bottom-screen Library (1C in the mockup).
 *
 * Recently played contains the full deduplicated history. The four-tile shelf
 * is only a preview; those same entries remain available here.
 */

enum {
	LIST_BTN_BACK = 100, /* well clear of the player's ids */
	LIST_RECENT0,         /* .. LIST_RECENT0 + RECENTS_MAX - 1 */
	LIST_PLAYLIST0 = 200, /* .. LIST_PLAYLIST0 + PLAYLISTS_MAX - 1 */
	LIST_ARM_PLAY = 300,
	LIST_ALBUM0 = 400, /* .. LIST_ALBUM0 + ALBUMS_MAX - 1 */
};

#define LIST_HEADER_H 30.0f
#define LIST_ROW_H    42.0f
#define LIST_ARMED_ROW_H 48.0f

typedef struct {
	C2D_TextBuf          buf;
	touch_builder       *tb;
	const recent_list   *recents;
	const playlist_list *playlists;
	const album_list    *albums;

	float scroll;     /* pixels scrolled down */
	int   pressed_id; /* control under the finger, or -1 */
	int   armed_id;   /* row awaiting PLAY confirmation, or -1 */
} screen_list_args;

void screen_list_draw(const screen_list_args *a);

/* How far the combined document can scroll. */
float screen_list_max_scroll(int recent_count, int playlist_count,
                             int album_count, int armed_id);

/* Adjust scroll just enough to keep a target row visible, using armed_id for
 * the document's expanded-row geometry. */
float screen_list_reveal_row(int recent_count, int playlist_count,
                             int album_count, int target_id, int armed_id,
                             float scroll);

/* Jump to the previous (-1) or next (+1) section caption. */
float screen_list_jump_section(int recent_count, int playlist_count,
                               int album_count, float scroll, int direction);

/* First row for a section caption currently aligned under the fixed header. */
int screen_list_section_first_id(int recent_count, int playlist_count,
                                 int album_count, float scroll);
