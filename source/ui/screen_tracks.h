#pragma once

#include <citro2d.h>

#include "../spotify/tracks.h"
#include "touch.h"

enum {
	TRACK_BTN_BACK = 500,
	TRACK_BTN_PREV_PAGE,
	TRACK_BTN_NEXT_PAGE,
	TRACK_BTN_RETRY,
	TRACK_BTN_SEARCH,
	TRACK_ROW0 = 600, /* .. TRACK_ROW0 + TRACK_PAGE_MAX - 1 */
	TRACK_QUEUE0 = 700, /* .. TRACK_QUEUE0 + TRACK_PAGE_MAX - 1 */
	TRACK_PLAY0 = 800, /* .. TRACK_PLAY0 + TRACK_PAGE_MAX - 1 */
};

typedef struct {
	C2D_TextBuf     buf;
	touch_builder  *tb;
	const track_page *page;
	const char      *collection_name;
	const char      *back_label;
	const char      *current_track_uri;
	const char      *error;
	bool             playing;
	unsigned         animation_ms;
	long             elapsed_ms;
	long             duration_ms;
	bool             loading;
	bool             ready;
	bool             search_mode;
	const char      *search_query;
	int              search_scanned;
	int              search_source_total;
	int              search_matched_total;
	bool             search_truncated;
	unsigned         search_animation_ms;
	bool             no_matches;
	float            scroll;
	int              pressed_id;
	int              armed_id;
} screen_tracks_args;

void  screen_tracks_draw(const screen_tracks_args *a);
float screen_tracks_max_scroll(int count, int armed_id);
float screen_tracks_reveal_row(int count, int row_id, int armed_id,
                               float scroll);
