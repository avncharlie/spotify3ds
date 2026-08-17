#pragma once

#include <citro2d.h>
#include <stdbool.h>
#include <stddef.h>

#include "../spotify/art.h"
#include "../spotify/lyrics.h"
#include "touch.h"

enum {
	LYRICS_BTN_BACK = 1700,
	LYRICS_BTN_FOLLOW,
	LYRICS_BTN_RETRY,
	LYRICS_LINE0
};

typedef struct {
	float          *tops;
	float          *heights;
	unsigned short *rows;
	size_t          count;
	float           document_height;
} lyrics_layout;

typedef struct {
	C2D_TextBuf         buf;
	const lyrics_doc   *doc;
	const lyrics_layout *layout;
	const album_art    *art;
	const char         *track;
	long                elapsed_ms;
	long                duration_ms;
	int                 highlight;
	bool                loading;
	bool                error;
	const char         *status;
	size_t              loading_received;
	size_t              loading_total;
	bool                loading_total_known;
	unsigned            loading_animation_ms;
	float               scroll;
	bool                follow;
	int                 pressed_id;
	touch_builder      *tb;
} screen_lyrics_args;

/* Initialize the destination to zero before its first build. Rebuilding an
 * existing layout replaces its arrays only after all new allocations succeed. */
bool lyrics_layout_build(lyrics_layout *layout, C2D_TextBuf buf,
	                     const lyrics_doc *doc);
void lyrics_layout_free(lyrics_layout *layout);

float screen_lyrics_max_scroll(const screen_lyrics_args *a);
float screen_lyrics_follow_scroll(const screen_lyrics_args *a);
void screen_lyrics_bottom_draw(const screen_lyrics_args *a);
