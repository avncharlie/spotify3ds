#include "screen_list.h"

#include <stdio.h>
#include <string.h>

#include "thumbs.h"
#include "search_popover.h"
#include "ui.h"

#define BOT_W 320.0f
#define BOT_H 240.0f
#define CONTENT_BOTTOM (BOT_H - UI_PROGRESS_BAR_H)

#define PAD_X    16.0f
#define THUMB    30.0f
#define THUMB_GAP 10.0f

/* Permanent play action immediately before the navigation gutter. */
#define PLAY_X 246.0f
#define PLAY_W 34.0f

/* Permanent navigation column. The scroll indicator starts exactly where this
 * ends, so every collection row presents the same harmless drill-down target. */
#define CHEVRON_X 280.0f
#define CHEVRON_W 34.0f

/* Scrollable document geometry below the fixed header. */
#define CAPTION_H  20.0f
#define DIVIDER_H  1.0f
#define SECTION_GAP 6.0f

/* Scroll indicator, per the mockup: a thin track at right: 3px. */
#define IND_X 314.0f
#define IND_W 3.0f
#define IND_Y 34.0f
#define IND_H 202.0f

#define CLR_HEADER   C2D_Color32(0x11, 0x11, 0x11, 0xFF)
#define CLR_ROW_ARM  C2D_Color32(0x1B, 0x1B, 0x1B, 0xFF)
#define CLR_NOW_BG   C2D_Color32(0x10, 0x16, 0x0F, 0xFF)
#define CLR_ROW_PRESS C2D_Color32(0x24, 0x24, 0x24, 0xFF)
#define CLR_GREEN    C2D_Color32(0x1D, 0xB9, 0x54, 0xFF)
#define CLR_GREEN_PRESS C2D_Color32(0x28, 0xD8, 0x68, 0xFF)
#define CLR_ACTION   C2D_Color32(0x08, 0x08, 0x08, 0xFF)
#define CLR_NAME     C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define CLR_SUB      C2D_Color32(0x8A, 0x8A, 0x8A, 0xFF)
#define CLR_CAPTION  C2D_Color32(0x8A, 0x8A, 0x8A, 0xFF)
#define CLR_COUNT    C2D_Color32(0x5C, 0x5C, 0x5C, 0xFF)
#define CLR_THUMB_BG C2D_Color32(0x22, 0x22, 0x2A, 0xFF)
#define CLR_DIVIDER  C2D_Color32(0x26, 0x26, 0x26, 0xFF)
#define CLR_IND_TRK  C2D_Color32(0x26, 0x26, 0x26, 0xFF)
#define CLR_IND_THMB C2D_Color32(0x7A, 0x7A, 0x7A, 0xFF)
#define CLR_PLAY_CELL C2D_Color32(0x16, 0x16, 0x16, 0xFF)
#define CLR_GUTTER   C2D_Color32(0x10, 0x10, 0x10, 0xFF)
#define CLR_GUTTER_PRESS C2D_Color32(0x20, 0x20, 0x20, 0xFF)
#define CLR_CHEVRON  C2D_Color32(0x8A, 0x8A, 0x8A, 0xFF)

static float document_top(bool filtering)
{
	return LIST_HEADER_H + (filtering ? LIST_FILTER_H : 0.0f);
}

static float viewport_h(bool filtering)
{
	return CONTENT_BOTTOM - document_top(filtering);
}

static bool armed_valid(int id, int recent_count, int playlist_count,
                        int album_count)
{
	if (id >= LIST_RECENT0 && id < LIST_RECENT0 + recent_count)
		return true;
	if (id >= LIST_PLAYLIST0 && id < LIST_PLAYLIST0 + playlist_count)
		return true;
	return id >= LIST_ALBUM0 && id < LIST_ALBUM0 + album_count;
}

static float row_h(int id, int armed_id)
{
	return id == armed_id ? LIST_ARMED_ROW_H : LIST_ROW_H;
}

static float content_h(int recent_count, int playlist_count, int album_count,
                       int armed_id)
{
	float h = 0.0f;
	int sections = 0;
#define ADD_SECTION(count) do { \
	if ((count) > 0) { \
		if (sections++) h += DIVIDER_H + SECTION_GAP; \
		h += CAPTION_H + (float)(count) * LIST_ROW_H; \
	} \
} while (0)
	ADD_SECTION(recent_count);
	ADD_SECTION(playlist_count);
	ADD_SECTION(album_count);
#undef ADD_SECTION
	if (armed_valid(armed_id, recent_count, playlist_count, album_count))
		h += LIST_ARMED_ROW_H - LIST_ROW_H;

	return h;
}

float screen_list_max_scroll(int recent_count, int playlist_count,
                             int album_count, int armed_id,
                             bool filtering)
{
	const float content =
	    content_h(recent_count, playlist_count, album_count, armed_id);
	const float max     = content - viewport_h(filtering);
	return max > 0.0f ? max : 0.0f;
}

static bool row_bounds(int recent_count, int playlist_count, int album_count,
                       int target_id, int armed_id, float *top, float *bottom,
                       bool filtering)
{
	float y = document_top(filtering);
	bool have_section = false;

	if (recent_count > 0) {
		have_section = true;
		y += CAPTION_H;
		for (int i = 0; i < recent_count; i++) {
			const int id = LIST_RECENT0 + i;
			const float h = row_h(id, armed_id);
			if (id == target_id) {
				*top = y;
				*bottom = y + h;
				return true;
			}
			y += h;
		}
	}

	if (playlist_count > 0) {
		if (have_section)
			y += DIVIDER_H + SECTION_GAP;
		have_section = true;
		y += CAPTION_H;
		for (int i = 0; i < playlist_count; i++) {
			const int id = LIST_PLAYLIST0 + i;
			const float h = row_h(id, armed_id);
			if (id == target_id) {
				*top = y;
				*bottom = y + h;
				return true;
			}
			y += h;
		}
	}

	if (album_count > 0) {
		if (have_section)
			y += DIVIDER_H + SECTION_GAP;
		y += CAPTION_H;
		for (int i = 0; i < album_count; i++) {
			const int id = LIST_ALBUM0 + i;
			const float h = row_h(id, armed_id);
			if (id == target_id) {
				*top = y;
				*bottom = y + h;
				return true;
			}
			y += h;
		}
	}

	return false;
}

float screen_list_reveal_row(int recent_count, int playlist_count,
                             int album_count, int target_id, int armed_id,
                             float scroll, bool filtering)
{
	float top, bottom;
	if (!row_bounds(recent_count, playlist_count, album_count, target_id,
	                armed_id, &top, &bottom, filtering))
		return scroll;

	const float doc_top = document_top(filtering);
	if (top < scroll + doc_top)
		scroll = top - doc_top;
	else if (bottom > scroll + CONTENT_BOTTOM)
		scroll = bottom - CONTENT_BOTTOM;

	const float max =
	    screen_list_max_scroll(recent_count, playlist_count, album_count,
	                           armed_id, filtering);
	if (scroll < 0.0f)
		scroll = 0.0f;
	if (scroll > max)
		scroll = max;
	return scroll;
}

float screen_list_jump_section(int recent_count, int playlist_count,
                               int album_count, float scroll, int direction,
                               bool filtering)
{
	float targets[3];
	int n = 0;
	const float doc_top = document_top(filtering);
	float y = doc_top;

	if (recent_count > 0) {
		targets[n++] = y - doc_top;
		y += CAPTION_H + (float)recent_count * LIST_ROW_H;
	}
	if (playlist_count > 0) {
		if (n)
			y += DIVIDER_H + SECTION_GAP;
		targets[n++] = y - doc_top;
		y += CAPTION_H + (float)playlist_count * LIST_ROW_H;
	}
	if (album_count > 0) {
		if (n)
			y += DIVIDER_H + SECTION_GAP;
		targets[n++] = y - doc_top;
	}
	if (!n)
		return 0.0f;

	float target = direction > 0 ? targets[n - 1] : targets[0];
	if (direction > 0) {
		for (int i = 0; i < n; i++) {
			if (targets[i] > scroll + 0.5f) {
				target = targets[i];
				break;
			}
		}
	} else {
		for (int i = n - 1; i >= 0; i--) {
			if (targets[i] < scroll - 0.5f) {
				target = targets[i];
				break;
			}
		}
	}

	const float max =
	    screen_list_max_scroll(recent_count, playlist_count, album_count, -1,
	                           filtering);
	return target > max ? max : target;
}

int screen_list_section_first_id(int recent_count, int playlist_count,
                                 int album_count, float scroll,
                                 bool filtering)
{
	const float doc_top = document_top(filtering);
	float y = doc_top;
	int sections = 0;
	if (recent_count > 0) {
		if (scroll > -0.5f && scroll < 0.5f)
			return LIST_RECENT0;
		y += CAPTION_H + (float)recent_count * LIST_ROW_H;
		sections++;
	}

	if (playlist_count > 0) {
		if (sections)
			y += DIVIDER_H + SECTION_GAP;
		const float playlist_scroll = y - doc_top;
		if (scroll > playlist_scroll - 0.5f &&
		    scroll < playlist_scroll + 0.5f)
			return LIST_PLAYLIST0;
		y += CAPTION_H + (float)playlist_count * LIST_ROW_H;
		sections++;
	}

	if (album_count <= 0)
		return -1;
	if (sections)
		y += DIVIDER_H + SECTION_GAP;
	float album_scroll = y - doc_top;
	const float max =
	    screen_list_max_scroll(recent_count, playlist_count, album_count, -1,
	                           filtering);
	if (album_scroll > max)
		album_scroll = max;
	if (album_count > 0 && scroll > album_scroll - 0.5f &&
	    scroll < album_scroll + 0.5f)
		return LIST_ALBUM0;

	return -1;
}

static void add_clipped_hit(touch_builder *tb, float x, float y, float w,
                            float h, int id, float clip_top)
{
	const float top = y < clip_top ? clip_top : y;
	const float bottom = y + h > CONTENT_BOTTOM ? CONTENT_BOTTOM : y + h;
	if (bottom - top > 8.0f)
		tb_add(tb, x, top, w, bottom - top, id);
}

static int chevron_id_for_row(int id)
{
	if (id >= LIST_RECENT0 && id < LIST_RECENT0 + RECENTS_MAX)
		return LIST_CHEVRON_RECENT0 + id - LIST_RECENT0;
	if (id >= LIST_PLAYLIST0 && id < LIST_PLAYLIST0 + PLAYLISTS_MAX)
		return LIST_CHEVRON_PLAYLIST0 + id - LIST_PLAYLIST0;
	if (id >= LIST_ALBUM0 && id < LIST_ALBUM0 + ALBUMS_MAX)
		return LIST_CHEVRON_ALBUM0 + id - LIST_ALBUM0;
	return -1;
}

static int play_id_for_row(int id)
{
	if (id >= LIST_RECENT0 && id < LIST_RECENT0 + RECENTS_MAX)
		return LIST_PLAY_RECENT0 + id - LIST_RECENT0;
	if (id >= LIST_PLAYLIST0 && id < LIST_PLAYLIST0 + PLAYLISTS_MAX)
		return LIST_PLAY_PLAYLIST0 + id - LIST_PLAYLIST0;
	if (id >= LIST_ALBUM0 && id < LIST_ALBUM0 + ALBUMS_MAX)
		return LIST_PLAY_ALBUM0 + id - LIST_ALBUM0;
	return -1;
}

static void draw_caption(const screen_list_args *a, float y, const char *label,
                         int count)
{
	const float clip_top = document_top(a->search_query && a->search_query[0]);
	if (y >= CONTENT_BOTTOM || y + CAPTION_H <= clip_top)
		return;

	C2D_DrawRectSolid(0.0f, y, 0.0f, BOT_W, CAPTION_H, CLR_HEADER);

	const float top = y + (CAPTION_H - ui_px(TY_MICRO)) / 2.0f;
	const float bl  = ui_baseline(top, TY_MICRO);
	ui_text_tracked(a->buf, label, PAD_X, bl, TY_MICRO, 1.1f, CLR_CAPTION);

	if (count >= 0) {
		char text[16];
		snprintf(text, sizeof text, "%d", count);
		const float w = ui_text_width(a->buf, text, TY_MICRO);
		ui_text(a->buf, text, BOT_W - PAD_X - w, bl, TY_MICRO, w, CLR_COUNT);
	}
}

static void draw_row(const screen_list_args *a, const collection_item *item,
                     float y, int id)
{
	const bool armed = id == a->armed_id;
	const bool filtering = a->search_query && a->search_query[0];
	const bool current = a->current_context_uri &&
	                     a->current_context_uri[0] &&
	                     strcmp(item->context_uri,
	                            a->current_context_uri) == 0;
	const float h = row_h(id, a->armed_id);
	if (y >= CONTENT_BOTTOM || y + h <= LIST_HEADER_H)
		return;

	const bool pressed = a->pressed_id == id;
	const int chevron_id = chevron_id_for_row(id);
	const int play_id = play_id_for_row(id);
	const bool chevron_pressed = a->pressed_id == chevron_id;
	const bool play_pressed = a->pressed_id == play_id;
	if (current) {
		C2D_DrawRectSolid(0.0f, y, 0.0f, BOT_W, h, CLR_NOW_BG);
	} else if (armed) {
		C2D_DrawRectSolid(0.0f, y, 0.0f, BOT_W, h, CLR_ROW_ARM);
	} else if (pressed) {
		C2D_DrawRectSolid(0.0f, y, 0.0f, BOT_W, h, CLR_ROW_PRESS);
	}
	if (current || armed)
		C2D_DrawRectSolid(0.0f, y, 0.0f, 3.0f, h, CLR_GREEN);
	C2D_DrawRectSolid(PLAY_X, y, 0.0f, PLAY_W, h,
	                  play_pressed ? CLR_GUTTER_PRESS : CLR_PLAY_CELL);
	const float pcx = PLAY_X + PLAY_W / 2.0f;
	const float pcy = y + h / 2.0f;
	const u32 play_clr = play_pressed ? CLR_GREEN_PRESS : CLR_GREEN;
	if (current && a->playing) {
		C2D_DrawRectSolid(pcx - 5.0f, pcy - 6.0f, 0.0f, 3.0f, 12.0f,
		                  play_clr);
		C2D_DrawRectSolid(pcx + 2.0f, pcy - 6.0f, 0.0f, 3.0f, 12.0f,
		                  play_clr);
	} else {
		C2D_DrawTriangle(pcx - 4.0f, pcy - 6.0f, play_clr,
		                 pcx - 4.0f, pcy + 6.0f, play_clr,
		                 pcx + 5.0f, pcy, play_clr, 0.0f);
	}

	C2D_DrawRectSolid(CHEVRON_X, y, 0.0f, CHEVRON_W, h,
	                  chevron_pressed ? CLR_GUTTER_PRESS : CLR_GUTTER);
	const float ccx = CHEVRON_X + 18.0f;
	const float ccy = y + h / 2.0f;
	const u32 chevron_clr = chevron_pressed ? CLR_GREEN_PRESS : CLR_CHEVRON;
	C2D_DrawLine(ccx - 5.0f, ccy - 7.0f, chevron_clr, ccx + 2.0f, ccy,
	             chevron_clr, 3.0f, 0.0f);
	C2D_DrawLine(ccx + 2.0f, ccy, chevron_clr, ccx - 5.0f, ccy + 7.0f,
	             chevron_clr, 3.0f, 0.0f);

	const float thumb = THUMB;
	const float ty = y + (h - thumb) / 2.0f;
	const C2D_Image *art = thumbs_get(item->art_url);
	if (art) {
		const float sx = thumb / (float)art->subtex->width;
		const float sy = thumb / (float)art->subtex->height;
		C2D_DrawImageAt(*art, PAD_X, ty, 0.0f, NULL, sx, sy);
	} else {
		C2D_DrawRectSolid(PAD_X, ty, 0.0f, thumb, thumb, CLR_THUMB_BG);
	}
	if (current)
		ui_now_playing_badge(PAD_X, ty, thumb, a->playing, a->animation_ms);

	const float tx = PAD_X + thumb + THUMB_GAP;
	const float tw = PLAY_X - tx - 7.0f;
	const float name_h = ui_px(TY_ROW_NAME);
	const float sub_h  = ui_px(TY_ROW_SUB);
	const float gap    = 2.0f;
	const float top = y + (h - (name_h + gap + sub_h)) / 2.0f;

	ui_text_highlight(a->buf, item->name, a->search_query, tx,
	                  ui_baseline(top, TY_ROW_NAME), TY_ROW_NAME, tw,
	                  current ? CLR_GREEN : CLR_NAME, CLR_GREEN);
	ui_text_highlight(a->buf, item->subtitle, a->search_query, tx,
	                  ui_baseline(top + name_h + gap, TY_ROW_SUB), TY_ROW_SUB,
	                  tw, CLR_SUB, CLR_GREEN);

	/* Keep all three zones disjoint: row body, immediate play, and drill-down. */
	if (a->suppress_hits)
		return;
	add_clipped_hit(a->tb, PLAY_X, y, PLAY_W, h, play_id,
	                document_top(filtering));
	add_clipped_hit(a->tb, CHEVRON_X, y, CHEVRON_W, h, chevron_id,
	                document_top(filtering));
	add_clipped_hit(a->tb, 0.0f, y, PLAY_X, h, id,
	                document_top(filtering));
}

void screen_list_draw(const screen_list_args *a)
{
	const int recent_count = a->recents ? a->recents->count : 0;
	const int playlist_count = a->playlists ? a->playlists->count : 0;
	const int album_count = a->albums ? a->albums->count : 0;
	const int rn = recent_count;
	const bool filtering = a->search_query && a->search_query[0];
	float y = document_top(filtering) - a->scroll;
	bool have_section = false;

	/* Everything scrollable is drawn before the fixed header because citro2d
	 * has no scissor rectangle. */
	if (rn > 0) {
		have_section = true;
		draw_caption(a, y, "RECENTLY PLAYED", -1);
		y += CAPTION_H;

		for (int i = 0; i < rn; i++) {
			const int id = LIST_RECENT0 + i;
			draw_row(a, &a->recents->items[i], y, id);
			y += row_h(id, a->armed_id);
		}

	}

	if (playlist_count > 0) {
		if (have_section) {
			C2D_DrawRectSolid(0.0f, y, 0.0f, BOT_W, DIVIDER_H, CLR_DIVIDER);
			y += DIVIDER_H + SECTION_GAP;
		}
		have_section = true;
		draw_caption(a, y, "PLAYLISTS",
		             a->playlists ? a->playlists->total : playlist_count);
		y += CAPTION_H;
		for (int i = 0; i < playlist_count; i++) {
			const int id = LIST_PLAYLIST0 + i;
			draw_row(a, &a->playlists->items[i], y, id);
			y += row_h(id, a->armed_id);
		}
	}

	if (album_count > 0) {
		if (have_section) {
			C2D_DrawRectSolid(0.0f, y, 0.0f, BOT_W, DIVIDER_H, CLR_DIVIDER);
			y += DIVIDER_H + SECTION_GAP;
		}
		draw_caption(a, y, "ALBUMS", a->albums ? a->albums->total : album_count);
		y += CAPTION_H;
		for (int i = 0; i < album_count; i++) {
			const int id = LIST_ALBUM0 + i;
			draw_row(a, &a->albums->items[i], y, id);
			y += row_h(id, a->armed_id);
		}
	}
	if (filtering && playlist_count + album_count == 0) {
		ui_text(a->buf, "No matches", PAD_X, ui_baseline(y + 34, TY_ROW_NAME),
		        TY_ROW_NAME, BOT_W - 2 * PAD_X, CLR_SUB);
	}

	/* --- scroll indicator ---------------------------------------------- */
	const float max_scroll =
	    screen_list_max_scroll(recent_count, playlist_count, album_count,
	                           a->armed_id, filtering);
	if (max_scroll > 0.0f) {
		const float ind_y = document_top(filtering) + 4.0f;
		const float ind_h = BOT_H - ind_y - 4.0f;
		C2D_DrawRectSolid(IND_X, ind_y, 0.0f, IND_W, ind_h, CLR_IND_TRK);

		float th = ind_h * viewport_h(filtering) /
		           content_h(recent_count, playlist_count, album_count,
		                     a->armed_id);
		if (th < 20.0f)
			th = 20.0f;
		const float ty = ind_y + (ind_h - th) * (a->scroll / max_scroll);
		C2D_DrawRectSolid(IND_X, ty, 0.0f, IND_W, th, CLR_IND_THMB);
	}

	if (filtering) {
		const float fy = LIST_HEADER_H;
		C2D_DrawRectSolid(0, fy, 0, BOT_W, LIST_FILTER_H,
		                  C2D_Color32(0x1B, 0x1B, 0x1B, 0xFF));
		ui_disc(17, fy + 11, 5, CLR_GREEN);
		ui_disc(17, fy + 11, 3, C2D_Color32(0x1B, 0x1B, 0x1B, 0xFF));
		C2D_DrawLine(20.5f, fy + 14.5f, CLR_GREEN, 25, fy + 19, CLR_GREEN,
		             2.0f, 0.0f);
		ui_text(a->buf, a->search_query, 34,
		        ui_baseline(fy + (LIST_FILTER_H - ui_px(TY_ROW_NAME)) / 2,
		                    TY_ROW_NAME),
		        TY_ROW_NAME, 150, CLR_NAME);
		char matches[24];
		snprintf(matches, sizeof matches, "%d matches", a->search_matches);
		const float mw = ui_text_width(a->buf, matches, TY_ROW_SUB);
		ui_text(a->buf, matches, 274 - mw,
		        ui_baseline(fy + (LIST_FILTER_H - ui_px(TY_ROW_SUB)) / 2,
		                    TY_ROW_SUB),
		        TY_ROW_SUB, mw, CLR_SUB);
		C2D_DrawRectSolid(280, fy + 2, 0, 34, LIST_FILTER_H - 4,
		                  a->pressed_id == LIST_BTN_CLEAR_SEARCH
		                      ? CLR_GUTTER_PRESS
		                      : CLR_GUTTER);
		const u32 xclr = a->pressed_id == LIST_BTN_CLEAR_SEARCH
		                     ? CLR_GREEN_PRESS
		                     : CLR_NAME;
		C2D_DrawLine(291, fy + 8, xclr, 303, fy + 20, xclr, 2.5f, 0);
		C2D_DrawLine(303, fy + 8, xclr, 291, fy + 20, xclr, 2.5f, 0);
		tb_add(a->tb, 280, fy, 34, LIST_FILTER_H, LIST_BTN_CLEAR_SEARCH);
	}

	/* --- header, last so it covers scrolled rows ------------------------ */
	C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, BOT_W, LIST_HEADER_H, CLR_HEADER);

	const bool back_pressed = a->pressed_id == LIST_BTN_BACK;
	const u32  back_clr =
	    back_pressed ? C2D_Color32(0x1D, 0xB9, 0x54, 0xFF) : CLR_NAME;

	/* Left-pointing triangle. */
	const float ax = PAD_X, ay = LIST_HEADER_H / 2.0f;
	C2D_DrawTriangle(ax, ay, back_clr, ax + 7.0f, ay - 5.0f, back_clr,
	                 ax + 7.0f, ay + 5.0f, back_clr, 0.0f);

	/* The search disc starts at x=292, so the title has the header to itself
	 * up to a small gap before it. */
	ui_text(a->buf, "Library", ax + 17.0f,
	        ui_baseline(LIST_HEADER_H / 2.0f - ui_px(TY_ROW_NAME) / 2.0f,
	                    TY_ROW_NAME),
	        TY_ROW_NAME, 264.0f - (ax + 17.0f), CLR_NAME);

	/* Same affordance as the Tracks header, so one green disc means "search"
	 * everywhere in the app. */
	const bool find_pressed = a->pressed_id == LIST_BTN_FIND;
	const u32 find_bg = find_pressed ? CLR_GREEN_PRESS : CLR_GREEN;
	ui_disc(302, 15, 10, find_bg);
	ui_disc(300, 13, 4, CLR_ACTION);
	ui_disc(300, 13, 2, find_bg);
	C2D_DrawLine(303, 16, CLR_ACTION, 307, 20, CLR_ACTION, 2, 0);
	search_popover_draw_ring(302.0f, 15.0f, a->hold_progress);

	/* Generous hit area: the arrow itself is small. */
	tb_add(a->tb, 0.0f, 0.0f, 90.0f, LIST_HEADER_H, LIST_BTN_BACK);
	tb_add(a->tb, 280.0f, 0.0f, 40.0f, LIST_HEADER_H, LIST_BTN_FIND);

	ui_progress_bar(a->elapsed_ms, a->duration_ms);
}
