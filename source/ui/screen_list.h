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
	LIST_ALBUM0 = 400, /* .. LIST_ALBUM0 + ALBUMS_MAX - 1 */
	LIST_CHEVRON_RECENT0 = 700,
	LIST_CHEVRON_PLAYLIST0 = 800,
	LIST_CHEVRON_ALBUM0 = 1000,
	LIST_BTN_FIND = 1200,
	LIST_BTN_CLEAR_SEARCH,
	LIST_PLAY_RECENT0 = 1300,
	LIST_PLAY_PLAYLIST0 = 1400,
	LIST_PLAY_ALBUM0 = 1500,
};

#define LIST_HEADER_H 30.0f
#define LIST_FILTER_H 26.0f
#define LIST_ROW_H    42.0f
#define LIST_ARMED_ROW_H LIST_ROW_H

typedef struct {
	C2D_TextBuf          buf;
	touch_builder       *tb;
	const recent_list   *recents;
	const playlist_list *playlists;
	const album_list    *albums;
	const char          *current_context_uri;
	const char          *search_query;
	int                  search_matches;
	bool                 playing;
	unsigned             animation_ms;
	long                 elapsed_ms;
	long                 duration_ms;

	float scroll;     /* pixels scrolled down */
	int   pressed_id; /* control under the finger, or -1 */
	/* 0..1 of the way to a long press on the search disc; 0 draws nothing. */
	float hold_progress;
	/* The recent-searches panel is over the top and owns the touch, so the
	 * rows must not register hit areas: they would compete for a 32-rect
	 * budget that silently drops the overflow. */
	bool  suppress_hits;
	int   armed_id;   /* row awaiting PLAY confirmation, or -1 */
} screen_list_args;

void screen_list_draw(const screen_list_args *a);

/* How far the combined document can scroll. */
float screen_list_max_scroll(int recent_count, int playlist_count,
                             int album_count, int armed_id,
                             bool search_active);

/* Adjust scroll just enough to keep a target row visible, using armed_id for
 * the document's expanded-row geometry. */
float screen_list_reveal_row(int recent_count, int playlist_count,
                             int album_count, int target_id, int armed_id,
                             float scroll, bool search_active);

/* Jump to the previous (-1) or next (+1) section caption. */
float screen_list_jump_section(int recent_count, int playlist_count,
                               int album_count, float scroll, int direction,
                               bool search_active);

/* First row for a section caption currently aligned under the fixed header. */
int screen_list_section_first_id(int recent_count, int playlist_count,
                                 int album_count, float scroll,
                                 bool search_active);
