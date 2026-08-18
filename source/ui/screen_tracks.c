#include "screen_tracks.h"

#include <stdio.h>
#include <string.h>

#include "thumbs.h"
#include "ui.h"

#define BOT_W 320.0f
#define BOT_H 240.0f
#define CONTENT_BOTTOM (BOT_H - UI_PROGRESS_BAR_H)
#define HEADER_H 30.0f
#define CAPTION_H 20.0f
#define ROWS_TOP (HEADER_H + CAPTION_H)
#define ROW_H 38.0f
#define ROW_ARMED_H ROW_H
#define PAD_X 16.0f
#define THUMB 30.0f
#define THUMB_GAP 10.0f
#define PLAY_X 246.0f
#define PLAY_W 34.0f
#define QUEUE_X 280.0f
#define QUEUE_W 34.0f
#define IND_X 314.0f
#define IND_Y 54.0f
#define IND_W 3.0f
#define IND_H 182.0f

#define CLR_HEADER C2D_Color32(0x11, 0x11, 0x11, 0xFF)
#define CLR_ROW_ARM C2D_Color32(0x1B, 0x1B, 0x1B, 0xFF)
#define CLR_NOW_BG C2D_Color32(0x10, 0x16, 0x0F, 0xFF)
#define CLR_ROW_PRESS C2D_Color32(0x24, 0x24, 0x24, 0xFF)
#define CLR_GREEN C2D_Color32(0x1D, 0xB9, 0x54, 0xFF)
#define CLR_GREEN_PRESS C2D_Color32(0x28, 0xD8, 0x68, 0xFF)
#define CLR_ACTION C2D_Color32(0x08, 0x08, 0x08, 0xFF)
#define CLR_NAME C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define CLR_SUB C2D_Color32(0x8A, 0x8A, 0x8A, 0xFF)
#define CLR_DISABLED C2D_Color32(0x60, 0x60, 0x60, 0xFF)
#define CLR_CAPTION C2D_Color32(0x8A, 0x8A, 0x8A, 0xFF)
#define CLR_COUNT C2D_Color32(0x5C, 0x5C, 0x5C, 0xFF)
#define CLR_IND_TRK C2D_Color32(0x26, 0x26, 0x26, 0xFF)
#define CLR_IND_THMB C2D_Color32(0x7A, 0x7A, 0x7A, 0xFF)
#define CLR_ERROR C2D_Color32(0xFF, 0x6B, 0x5B, 0xFF)
#define CLR_THUMB_BG C2D_Color32(0x22, 0x22, 0x2A, 0xFF)
#define CLR_PLAY_CELL C2D_Color32(0x16, 0x16, 0x16, 0xFF)
#define CLR_GUTTER C2D_Color32(0x10, 0x10, 0x10, 0xFF)
#define CLR_GUTTER_PRESS C2D_Color32(0x20, 0x20, 0x20, 0xFF)

static float row_h(int id, int armed_id)
{
	return id == armed_id ? ROW_ARMED_H : ROW_H;
}

static float content_h(int count, int armed_id)
{
	float h = (float)count * ROW_H;
	if (armed_id >= TRACK_ROW0 && armed_id < TRACK_ROW0 + count)
		h += ROW_ARMED_H - ROW_H;
	return h;
}

float screen_tracks_max_scroll(int count, int armed_id)
{
	const float max = content_h(count, armed_id) - (CONTENT_BOTTOM - ROWS_TOP);
	return max > 0.0f ? max : 0.0f;
}

float screen_tracks_reveal_row(int count, int row_id, int armed_id,
	                           float scroll)
{
	const int idx = row_id - TRACK_ROW0;
	if (idx < 0 || idx >= count)
		return scroll;
	float top = ROWS_TOP;
	for (int i = 0; i < idx; i++)
		top += row_h(TRACK_ROW0 + i, armed_id);
	const float bottom = top + row_h(row_id, armed_id);
	if (top < scroll + ROWS_TOP)
		scroll = top - ROWS_TOP;
	else if (bottom > scroll + CONTENT_BOTTOM)
		scroll = bottom - CONTENT_BOTTOM;
	const float max = screen_tracks_max_scroll(count, armed_id);
	if (scroll < 0.0f)
		scroll = 0.0f;
	if (scroll > max)
		scroll = max;
	return scroll;
}

static void rounded_rect(float x, float y, float w, float h, float r, u32 clr)
{
	C2D_DrawRectSolid(x + r, y, 0.0f, w - 2.0f * r, h, clr);
	C2D_DrawRectSolid(x, y + r, 0.0f, w, h - 2.0f * r, clr);
	ui_disc(x + r, y + r, r, clr);
	ui_disc(x + w - r, y + r, r, clr);
	ui_disc(x + r, y + h - r, r, clr);
	ui_disc(x + w - r, y + h - r, r, clr);
}

static void clipped_hit(touch_builder *tb, float x, float y, float w, float h,
	                    int id)
{
	const float top = y < ROWS_TOP ? ROWS_TOP : y;
	const float bottom = y + h > CONTENT_BOTTOM ? CONTENT_BOTTOM : y + h;
	if (bottom - top > 8.0f)
		tb_add(tb, x, top, w, bottom - top, id);
}

static void draw_queue_icon(float cy, u32 clr)
{
	const float x = QUEUE_X + 9.0f;
	C2D_DrawRectSolid(x, cy - 8.0f, 0.0f, 13.0f, 2.0f, clr);
	C2D_DrawRectSolid(x, cy - 2.0f, 0.0f, 13.0f, 2.0f, clr);
	C2D_DrawRectSolid(x, cy + 4.0f, 0.0f, 7.0f, 2.0f, clr);
	C2D_DrawRectSolid(x + 13.0f, cy - 1.0f, 0.0f, 2.0f, 10.0f, clr);
	C2D_DrawRectSolid(x + 9.0f, cy + 3.0f, 0.0f, 10.0f, 2.0f, clr);
}

static void draw_row(const screen_tracks_args *a, const track_item *item,
	                 float y, int id)
{
	const bool armed = id == a->armed_id;
	const bool current = a->current_track_uri && a->current_track_uri[0] &&
	                     strcmp(item->uri, a->current_track_uri) == 0;
	const int play_id = TRACK_PLAY0 + id - TRACK_ROW0;
	const int queue_id = TRACK_QUEUE0 + id - TRACK_ROW0;
	const bool play_pressed = a->pressed_id == play_id;
	const bool queue_pressed = a->pressed_id == queue_id;
	const float h = row_h(id, a->armed_id);
	if (y >= CONTENT_BOTTOM || y + h <= HEADER_H)
		return;

	if (current) {
		C2D_DrawRectSolid(0, y, 0, BOT_W, h, CLR_NOW_BG);
	} else if (armed) {
		C2D_DrawRectSolid(0, y, 0, BOT_W, h, CLR_ROW_ARM);
	} else if (a->pressed_id == id) {
		C2D_DrawRectSolid(0, y, 0, BOT_W, h, CLR_ROW_PRESS);
	}
	if (current || armed)
		C2D_DrawRectSolid(0, y, 0, 3, h, CLR_GREEN);
	C2D_DrawRectSolid(PLAY_X, y, 0.0f, PLAY_W, h,
	                  play_pressed ? CLR_GUTTER_PRESS : CLR_PLAY_CELL);
	const float pcx = PLAY_X + PLAY_W / 2.0f;
	const float pcy = y + h / 2.0f;
	const u32 play_clr = !item->playable ? CLR_DISABLED
	                     : play_pressed  ? CLR_GREEN_PRESS
	                                      : CLR_GREEN;
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
	C2D_DrawRectSolid(QUEUE_X, y, 0.0f, QUEUE_W, h,
	                  queue_pressed ? CLR_GUTTER_PRESS : CLR_GUTTER);
	const u32 queue_clr = !item->playable ? CLR_DISABLED
	                      : queue_pressed ? CLR_GREEN_PRESS
	                      : armed         ? CLR_NAME
	                                      : CLR_SUB;
	draw_queue_icon(y + h / 2.0f, queue_clr);

	const u32 text_clr = item->playable ? CLR_NAME : CLR_DISABLED;
	const float thumb = THUMB;
	const float thumb_y = y + (h - thumb) / 2.0f;
	const C2D_Image *art = thumbs_get(item->art_url);
	if (art) {
		const float sx = thumb / (float)art->subtex->width;
		const float sy = thumb / (float)art->subtex->height;
		C2D_DrawImageAt(*art, PAD_X, thumb_y, 0.0f, NULL, sx, sy);
	} else {
		C2D_DrawRectSolid(PAD_X, thumb_y, 0.0f, thumb, thumb, CLR_THUMB_BG);
	}
	if (current)
		ui_now_playing_badge(PAD_X, thumb_y, thumb, a->playing,
		                     a->animation_ms);

	const float tx = PAD_X + thumb + THUMB_GAP;
	const float tw = PLAY_X - tx - 7.0f;
	const float name_h = ui_px(TY_ROW_NAME);
	const float sub_h = ui_px(TY_ROW_SUB);
	const float top = y + (h - name_h - sub_h - 2.0f) / 2.0f;
	ui_text_highlight(a->buf, item->name,
	                  a->search_mode ? a->search_query : NULL, tx,
	                  ui_baseline(top, TY_ROW_NAME), TY_ROW_NAME, tw,
	                  current ? CLR_GREEN : text_clr, CLR_GREEN);

	const char *subtitle = item->artist;
	char search_subtitle[260];
	if (a->search_mode && item->album[0]) {
		snprintf(search_subtitle, sizeof search_subtitle, "%s%s%s",
		         item->artist, item->artist[0] ? " - " : "", item->album);
		subtitle = search_subtitle;
	}
	if (item->is_local)
		subtitle = "Local file - unavailable";
	else if (item->kind == TRACK_ITEM_EPISODE)
		subtitle = "Episode - unavailable";
	else if (!item->playable)
		subtitle = item->artist[0] ? item->artist : "Unavailable";
	ui_text_highlight(a->buf, subtitle,
	                  a->search_mode ? a->search_query : NULL, tx,
	                  ui_baseline(top + name_h + 2.0f, TY_ROW_SUB), TY_ROW_SUB,
	                  tw, item->playable ? CLR_SUB : CLR_DISABLED, CLR_GREEN);

	if (item->playable)
		clipped_hit(a->tb, PLAY_X, y, PLAY_W, h, play_id);
	if (item->playable)
		clipped_hit(a->tb, QUEUE_X, y, QUEUE_W, h, queue_id);
	clipped_hit(a->tb, 0, y, PLAY_X, h, id);
}

void screen_tracks_draw(const screen_tracks_args *a)
{
	const int count = a->ready && a->page ? a->page->count : 0;
	float y = ROWS_TOP - a->scroll;

	if (a->ready && a->page && a->page->count > 0) {
		for (int i = 0; i < count; i++) {
			const int id = TRACK_ROW0 + i;
			draw_row(a, &a->page->items[i], y, id);
			y += row_h(id, a->armed_id);
		}

		const float max = screen_tracks_max_scroll(count, a->armed_id);
		if (max > 0) {
			C2D_DrawRectSolid(IND_X, IND_Y, 0, IND_W, IND_H, CLR_IND_TRK);
			float th = IND_H * (CONTENT_BOTTOM - ROWS_TOP) /
			           content_h(count, a->armed_id);
			if (th < 20)
				th = 20;
			const float ty = IND_Y + (IND_H - th) * (a->scroll / max);
			C2D_DrawRectSolid(IND_X, ty, 0, IND_W, th, CLR_IND_THMB);
		}
	} else {
		const char *message = a->loading
		                          ? (a->search_mode
		                                 ? (a->error && a->error[0]
		                                        ? a->error
		                                        : "Searching collection...")
		                                 : "Loading tracks...")
		                          : a->no_matches ? "No matches"
		                                          : a->error;
		if (!message || !message[0])
			message = "No tracks";
		const float maxw = BOT_W - 2 * PAD_X;
		ui_text(a->buf, message, PAD_X,
		        ui_baseline(106, TY_ROW_NAME), TY_ROW_NAME, maxw,
		        a->loading || a->no_matches ? CLR_SUB : CLR_ERROR);
		if (a->loading && a->search_mode) {
			char progress[64];
			snprintf(progress, sizeof progress, "%d / %d scanned - %d matches",
			         a->search_scanned, a->search_source_total,
			         a->search_matched_total);
			ui_text(a->buf, progress, PAD_X,
			        ui_baseline(128, TY_ROW_SUB), TY_ROW_SUB,
			        BOT_W - 2 * PAD_X, CLR_SUB);
			const float bar_x = PAD_X;
			const float bar_y = 151.0f;
			const float bar_w = BOT_W - 2 * PAD_X;
			C2D_DrawRectSolid(bar_x, bar_y, 0, bar_w, 5, CLR_IND_TRK);
			if (a->search_source_total > 0) {
				float ratio = (float)a->search_scanned /
				              (float)a->search_source_total;
				if (ratio > 1.0f)
					ratio = 1.0f;
				C2D_DrawRectSolid(bar_x, bar_y, 0, bar_w * ratio, 5, CLR_GREEN);
			} else {
				const float segment = 54.0f;
				const unsigned span = (unsigned)(bar_w - segment);
				const unsigned cycle = span
				    ? (a->search_animation_ms / 7) % (span * 2)
				    : 0;
				const float offset =
				    (float)(cycle <= span ? cycle : span * 2 - cycle);
				C2D_DrawRectSolid(bar_x + offset, bar_y, 0, segment, 5,
				                  CLR_GREEN);
			}
		}
		if (!a->loading && !a->no_matches && a->error && a->error[0]) {
			const bool pressed = a->pressed_id == TRACK_BTN_RETRY;
			rounded_rect(116, 135, 88, 34, 7,
			             pressed ? CLR_GREEN_PRESS : CLR_GREEN);
			ui_text(a->buf, "RETRY  X", 128,
			        ui_baseline(135 + (34 - ui_px(TY_ROW_NAME)) / 2, TY_ROW_NAME),
			        TY_ROW_NAME, 70, CLR_ACTION);
			tb_add(a->tb, 116, 135, 88, 34, TRACK_BTN_RETRY);
		}
	}

	/* Fixed page caption: rows are drawn first so this strip masks anything
	 * scrolling beneath it, just like the fixed navigation header above. */
	C2D_DrawRectSolid(0, HEADER_H, 0, BOT_W, CAPTION_H, CLR_HEADER);
	const char *caption = a->search_mode
	                          ? (a->loading ? "SEARCHING" : "RESULTS")
	                          : "TRACKS";
	ui_text_tracked(a->buf, caption, PAD_X,
	                ui_baseline(HEADER_H +
	                                (CAPTION_H - ui_px(TY_MICRO)) / 2,
	                            TY_MICRO),
	                TY_MICRO, 1.1f, CLR_CAPTION);
	if (a->search_mode && a->search_query && a->search_query[0])
		ui_text(a->buf, a->search_query, 96.0f,
		        ui_baseline(HEADER_H +
		                        (CAPTION_H - ui_px(TY_MICRO)) / 2,
		                    TY_MICRO),
		        TY_MICRO, 90.0f, CLR_GREEN);
	/* Rows have taken over the screen while the scan continues, so the caption
	 * strip carries the progress the placeholder panel would have shown. */
	if (a->search_mode && a->loading && a->ready) {
		const float bar_y = HEADER_H + CAPTION_H - 2.0f;
		C2D_DrawRectSolid(0, bar_y, 0, BOT_W, 2, CLR_IND_TRK);
		if (a->search_source_total > 0) {
			float ratio = (float)a->search_scanned /
			              (float)a->search_source_total;
			if (ratio > 1.0f)
				ratio = 1.0f;
			C2D_DrawRectSolid(0, bar_y, 0, BOT_W * ratio, 2, CLR_GREEN);
		}
	}
	if (a->ready && a->page) {
		char range[48];
		const int first = count ? a->page->offset + 1 : 0;
		const int last = a->page->offset + count;
		if (a->search_mode && a->search_truncated)
			snprintf(range, sizeof range, "%d-%d/%d (%d)", first, last,
			         a->page->total, a->search_matched_total);
		else
			snprintf(range, sizeof range, "%d-%d / %d", first, last,
			         a->page->total);
		const float rw = ui_text_width(a->buf, range, TY_MICRO);
		ui_text(a->buf, range, BOT_W - PAD_X - rw,
		        ui_baseline(HEADER_H +
		                        (CAPTION_H - ui_px(TY_MICRO)) / 2,
		                    TY_MICRO),
		        TY_MICRO, rw, CLR_COUNT);
	}

	C2D_DrawRectSolid(0, 0, 0, BOT_W, HEADER_H, CLR_HEADER);
	const bool back_pressed = a->pressed_id == TRACK_BTN_BACK;
	const u32 back_clr = back_pressed ? CLR_GREEN : CLR_NAME;
	C2D_DrawTriangle(PAD_X, 15, back_clr, PAD_X + 7, 10, back_clr,
	                 PAD_X + 7, 20, back_clr, 0);
	ui_text(a->buf, a->back_label ? a->back_label : "Library", PAD_X + 17,
	        ui_baseline((HEADER_H - ui_px(TY_ROW_NAME)) / 2, TY_ROW_NAME),
	        TY_ROW_NAME, 70, back_clr);
	tb_add(a->tb, 0, 0, 90, HEADER_H, TRACK_BTN_BACK);

	bool has_prev = false;
	bool has_next = false;
	if (a->ready && a->page) {
		has_prev =
		    a->page->offset > 0 ||
		    (a->page->collection.kind == COLLECTION_PLAYLIST &&
		     a->page->total > a->page->count);
		has_next =
		    a->page->offset + a->page->count < a->page->total ||
		    (a->page->collection.kind == COLLECTION_PLAYLIST &&
		     a->page->offset > 0 && a->page->total > a->page->count);
	}
	const float title_x = 96.0f;
	const float title_right = has_prev ? 190.0f
	                          : has_next ? 232.0f
	                                     : 274.0f;
	ui_text(a->buf, a->collection_name ? a->collection_name : "Tracks", title_x,
	        ui_baseline((HEADER_H - ui_px(TY_ROW_NAME)) / 2, TY_ROW_NAME),
	        TY_ROW_NAME, title_right - title_x, CLR_NAME);

	if (a->ready && a->page) {
		if (has_prev) {
			ui_text(a->buf, "< ZL", 196,
			        ui_baseline((HEADER_H - ui_px(TY_ROW_NAME)) / 2, TY_ROW_NAME),
			        TY_ROW_NAME, 38,
			        a->pressed_id == TRACK_BTN_PREV_PAGE ? CLR_GREEN : CLR_SUB);
			tb_add(a->tb, 194, 0, 40, HEADER_H, TRACK_BTN_PREV_PAGE);
		}
		if (has_next) {
			ui_text(a->buf, "ZR >", 238,
			        ui_baseline((HEADER_H - ui_px(TY_ROW_NAME)) / 2, TY_ROW_NAME),
			        TY_ROW_NAME, 38,
			        a->pressed_id == TRACK_BTN_NEXT_PAGE ? CLR_GREEN : CLR_SUB);
			tb_add(a->tb, 236, 0, 40, HEADER_H, TRACK_BTN_NEXT_PAGE);
		}
	}

	const bool search_pressed = a->pressed_id == TRACK_BTN_SEARCH;
	const u32 search_bg = search_pressed ? CLR_GREEN_PRESS : CLR_GREEN;
	ui_disc(302, 15, 10, search_bg);
	ui_disc(300, 13, 4, CLR_ACTION);
	ui_disc(300, 13, 2, search_bg);
	C2D_DrawLine(303, 16, CLR_ACTION, 307, 20, CLR_ACTION, 2, 0);
	tb_add(a->tb, 280, 0, 40, HEADER_H, TRACK_BTN_SEARCH);

	ui_progress_bar(a->elapsed_ms, a->duration_ms);
}
