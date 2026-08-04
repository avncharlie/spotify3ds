#include "screen_list.h"

#include <stdio.h>

#include "thumbs.h"
#include "ui.h"

#define BOT_W 320.0f
#define BOT_H 240.0f

#define PAD_X    16.0f
#define THUMB    30.0f
#define THUMB_ARMED 34.0f
#define THUMB_GAP 10.0f

/* Armed-row confirmation action. */
#define PLAY_X       240.0f
#define PLAY_W       64.0f
#define PLAY_H       36.0f

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

/* Visible height below the header. */
static float viewport_h(void)
{
	return BOT_H - LIST_HEADER_H;
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
	const int rn = recent_count;
	float h = CAPTION_H + (float)playlist_count * LIST_ROW_H + DIVIDER_H +
	          SECTION_GAP + CAPTION_H + (float)album_count * LIST_ROW_H;

	if (rn > 0)
		h += CAPTION_H + (float)rn * LIST_ROW_H + DIVIDER_H + SECTION_GAP;
	if (armed_valid(armed_id, recent_count, playlist_count, album_count))
		h += LIST_ARMED_ROW_H - LIST_ROW_H;

	return h;
}

float screen_list_max_scroll(int recent_count, int playlist_count,
                             int album_count, int armed_id)
{
	const float content =
	    content_h(recent_count, playlist_count, album_count, armed_id);
	const float max     = content - viewport_h();
	return max > 0.0f ? max : 0.0f;
}

static bool row_bounds(int recent_count, int playlist_count, int album_count,
                       int target_id, int armed_id, float *top, float *bottom)
{
	float y = LIST_HEADER_H;

	if (recent_count > 0) {
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
		y += DIVIDER_H + SECTION_GAP;
	}

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

	y += DIVIDER_H + SECTION_GAP + CAPTION_H;
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

	return false;
}

float screen_list_reveal_row(int recent_count, int playlist_count,
                             int album_count, int target_id, int armed_id,
                             float scroll)
{
	float top, bottom;
	if (!row_bounds(recent_count, playlist_count, album_count, target_id,
	                armed_id, &top, &bottom))
		return scroll;

	if (top < scroll + LIST_HEADER_H)
		scroll = top - LIST_HEADER_H;
	else if (bottom > scroll + BOT_H)
		scroll = bottom - BOT_H;

	const float max =
	    screen_list_max_scroll(recent_count, playlist_count, album_count,
	                           armed_id);
	if (scroll < 0.0f)
		scroll = 0.0f;
	if (scroll > max)
		scroll = max;
	return scroll;
}

float screen_list_jump_section(int recent_count, int playlist_count,
                               int album_count, float scroll, int direction)
{
	float targets[3];
	int n = 0;
	float y = LIST_HEADER_H;

	if (recent_count > 0) {
		targets[n++] = 0.0f;
		y += CAPTION_H + (float)recent_count * LIST_ROW_H + DIVIDER_H +
		     SECTION_GAP;
	}

	targets[n++] = y - LIST_HEADER_H;
	y += CAPTION_H + (float)playlist_count * LIST_ROW_H + DIVIDER_H +
	     SECTION_GAP;
	targets[n++] = y - LIST_HEADER_H;

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
	    screen_list_max_scroll(recent_count, playlist_count, album_count, -1);
	return target > max ? max : target;
}

int screen_list_section_first_id(int recent_count, int playlist_count,
                                 int album_count, float scroll)
{
	float y = LIST_HEADER_H;
	if (recent_count > 0) {
		if (scroll > -0.5f && scroll < 0.5f)
			return LIST_RECENT0;
		y += CAPTION_H + (float)recent_count * LIST_ROW_H + DIVIDER_H +
		     SECTION_GAP;
	}

	const float playlist_scroll = y - LIST_HEADER_H;
	if (playlist_count > 0 && scroll > playlist_scroll - 0.5f &&
	    scroll < playlist_scroll + 0.5f)
		return LIST_PLAYLIST0;

	y += CAPTION_H + (float)playlist_count * LIST_ROW_H + DIVIDER_H +
	     SECTION_GAP;
	float album_scroll = y - LIST_HEADER_H;
	const float max =
	    screen_list_max_scroll(recent_count, playlist_count, album_count, -1);
	if (album_scroll > max)
		album_scroll = max;
	if (album_count > 0 && scroll > album_scroll - 0.5f &&
	    scroll < album_scroll + 0.5f)
		return LIST_ALBUM0;

	return -1;
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

static void add_clipped_hit(touch_builder *tb, float x, float y, float w,
                            float h, int id)
{
	const float top = y < LIST_HEADER_H ? LIST_HEADER_H : y;
	const float bottom = y + h > BOT_H ? BOT_H : y + h;
	if (bottom - top > 8.0f)
		tb_add(tb, x, top, w, bottom - top, id);
}

static void draw_caption(const screen_list_args *a, float y, const char *label,
                         int count)
{
	if (y >= BOT_H || y + CAPTION_H <= LIST_HEADER_H)
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
	const float h = row_h(id, a->armed_id);
	if (y >= BOT_H || y + h <= LIST_HEADER_H)
		return;

	const bool pressed = a->pressed_id == id;
	if (armed) {
		C2D_DrawRectSolid(0.0f, y, 0.0f, BOT_W, h, CLR_ROW_ARM);
		C2D_DrawRectSolid(0.0f, y, 0.0f, 2.0f, h, CLR_GREEN);
	} else if (pressed) {
		C2D_DrawRectSolid(0.0f, y, 0.0f, BOT_W, h, CLR_ROW_PRESS);
	}

	const float thumb = armed ? THUMB_ARMED : THUMB;
	const float ty = y + (h - thumb) / 2.0f;
	const C2D_Image *art = thumbs_get(item->art_url);
	if (art) {
		const float sx = thumb / (float)art->subtex->width;
		const float sy = thumb / (float)art->subtex->height;
		C2D_DrawImageAt(*art, PAD_X, ty, 0.0f, NULL, sx, sy);
	} else {
		C2D_DrawRectSolid(PAD_X, ty, 0.0f, thumb, thumb, CLR_THUMB_BG);
	}

	const float tx = PAD_X + thumb + THUMB_GAP;
	const float tw = armed ? PLAY_X - tx - 7.0f : BOT_W - tx - PAD_X;
	const float name_h = ui_px(TY_ROW_NAME);
	const float sub_h  = ui_px(TY_ROW_SUB);
	const float gap    = 2.0f;
	const float top = y + (h - (name_h + gap + sub_h)) / 2.0f;

	ui_text(a->buf, item->name, tx, ui_baseline(top, TY_ROW_NAME), TY_ROW_NAME,
	        tw, CLR_NAME);
	ui_text(a->buf, item->subtitle, tx,
	        ui_baseline(top + name_h + gap, TY_ROW_SUB), TY_ROW_SUB, tw, CLR_SUB);

	if (armed) {
		const float ay = y + (h - PLAY_H) / 2.0f;
		const bool play_pressed = a->pressed_id == LIST_ARM_PLAY;
		rounded_rect(PLAY_X, ay, PLAY_W, PLAY_H, 7.0f,
		             play_pressed ? CLR_GREEN_PRESS : CLR_GREEN);

		/* Play triangle and label. */
		const float cy = y + h / 2.0f;
		C2D_DrawTriangle(PLAY_X + 8.0f, cy - 5.0f, CLR_ACTION,
		                 PLAY_X + 8.0f, cy + 5.0f, CLR_ACTION,
		                 PLAY_X + 15.0f, cy, CLR_ACTION, 0.0f);
		ui_text(a->buf, "PLAY", PLAY_X + 20.0f,
		        ui_baseline(cy - ui_px(TY_ROW_NAME) / 2.0f, TY_ROW_NAME),
		        TY_ROW_NAME, PLAY_W - 22.0f, CLR_ACTION);

		/* Register actions first: touch_hit returns the first overlapping rect. */
		add_clipped_hit(a->tb, PLAY_X, ay, PLAY_W, PLAY_H, LIST_ARM_PLAY);
	}

	/* A partially hidden row cannot be activated through the fixed header. */
	add_clipped_hit(a->tb, 0.0f, y, BOT_W, h, id);
}

void screen_list_draw(const screen_list_args *a)
{
	const int recent_count = a->recents ? a->recents->count : 0;
	const int playlist_count = a->playlists ? a->playlists->count : 0;
	const int album_count = a->albums ? a->albums->count : 0;
	const int rn = recent_count;
	float y = LIST_HEADER_H - a->scroll;

	/* Everything scrollable is drawn before the fixed header because citro2d
	 * has no scissor rectangle. */
	if (rn > 0) {
		draw_caption(a, y, "RECENTLY PLAYED", -1);
		y += CAPTION_H;

		for (int i = 0; i < rn; i++) {
			const int id = LIST_RECENT0 + i;
			draw_row(a, &a->recents->items[i], y, id);
			y += row_h(id, a->armed_id);
		}

		C2D_DrawRectSolid(0.0f, y, 0.0f, BOT_W, DIVIDER_H, CLR_DIVIDER);
		y += DIVIDER_H + SECTION_GAP;
	}

	draw_caption(a, y, "PLAYLISTS",
	             a->playlists ? a->playlists->total : playlist_count);
	y += CAPTION_H;

	for (int i = 0; i < playlist_count; i++) {
		const int id = LIST_PLAYLIST0 + i;
		draw_row(a, &a->playlists->items[i], y, id);
		y += row_h(id, a->armed_id);
	}

	C2D_DrawRectSolid(0.0f, y, 0.0f, BOT_W, DIVIDER_H, CLR_DIVIDER);
	y += DIVIDER_H + SECTION_GAP;

	draw_caption(a, y, "ALBUMS", a->albums ? a->albums->total : album_count);
	y += CAPTION_H;

	for (int i = 0; i < album_count; i++) {
		const int id = LIST_ALBUM0 + i;
		draw_row(a, &a->albums->items[i], y, id);
		y += row_h(id, a->armed_id);
	}

	/* --- scroll indicator ---------------------------------------------- */
	const float max_scroll =
	    screen_list_max_scroll(recent_count, playlist_count, album_count,
	                           a->armed_id);
	if (max_scroll > 0.0f) {
		C2D_DrawRectSolid(IND_X, IND_Y, 0.0f, IND_W, IND_H, CLR_IND_TRK);

		float th = IND_H * viewport_h() /
		           content_h(recent_count, playlist_count, album_count,
		                     a->armed_id);
		if (th < 20.0f)
			th = 20.0f;
		const float ty = IND_Y + (IND_H - th) * (a->scroll / max_scroll);
		C2D_DrawRectSolid(IND_X, ty, 0.0f, IND_W, th, CLR_IND_THMB);
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

	ui_text(a->buf, "Library", ax + 17.0f,
	        ui_baseline(LIST_HEADER_H / 2.0f - ui_px(TY_ROW_NAME) / 2.0f,
	                    TY_ROW_NAME),
	        TY_ROW_NAME, 240.0f, CLR_NAME);

	/* Generous hit area: the arrow itself is small. */
	tb_add(a->tb, 0.0f, 0.0f, 90.0f, LIST_HEADER_H, LIST_BTN_BACK);
}
