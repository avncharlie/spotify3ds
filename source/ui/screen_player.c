#include "screen_player.h"

#include <stdio.h>

#include "ui.h"

#define BOT_W 320.0f
#define BOT_H 240.0f

/* Shelf */
#define SHELF_LABEL_X 16.0f
#define SHELF_LABEL_Y 26.0f
#define SHELF_X       16.0f
#define SHELF_Y       42.0f
#define TILE          52.0f
#define TILE_GAP      7.0f

/* Transport: one baseline, no boxes. */
#define ROW_Y      152.0f
#define CTRL_GAP   22.0f
#define CTRL_SMALL 34.0f
#define PLAY_R     20.0f /* 40px disc */

#define CLR_WHITE   C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define CLR_DISC_FG C2D_Color32(0x0B, 0x0B, 0x0B, 0xFF)
#define CLR_IDLE    C2D_Color32(0xB3, 0xB3, 0xB3, 0xFF)
#define CLR_GREEN   C2D_Color32(0x1D, 0xB9, 0x54, 0xFF)
#define CLR_LABEL   C2D_Color32(0x6A, 0x6A, 0x6A, 0xFF)
#define CLR_TRACK   C2D_Color32(0x4D, 0x4D, 0x4D, 0xFF)
#define CLR_TILE    C2D_Color32(0x19, 0x19, 0x19, 0xFF)
#define CLR_TILE_BG C2D_Color32(0x22, 0x22, 0x2A, 0xFF)
#define CLR_BARS    C2D_Color32(0x8F, 0x8F, 0x8F, 0xFF)
#define CLR_ALL_TXT C2D_Color32(0xB3, 0xB3, 0xB3, 0xFF)
#define CLR_TIME    C2D_Color32(0xB3, 0xB3, 0xB3, 0xFF)

/* With the boxes gone there is no fill to highlight, so the glyph itself has
 * to carry the press. Dimming alone proved far too subtle on the small white
 * prev/next arrows, so a pressed control turns green - the accent already used
 * for "active" everywhere else - and gets a soft halo behind it, which is what
 * actually makes the tap feel like it landed. */
#define CLR_PRESS_HALO C2D_Color32(0x1D, 0xB9, 0x54, 0x33)

static u32 press_clr(u32 clr, bool pressed)
{
	return pressed ? CLR_GREEN : clr;
}

static void press_halo(float cx, float cy, bool pressed)
{
	if (pressed)
		ui_disc(cx, cy, 19.0f, CLR_PRESS_HALO);
}

static void tri_right(float x, float y, float w, float h, u32 clr)
{
	C2D_DrawTriangle(x, y, clr, x, y + h, clr, x + w, y + h / 2, clr, 0.0f);
}

static void tri_left(float x, float y, float w, float h, u32 clr)
{
	C2D_DrawTriangle(x + w, y, clr, x + w, y + h, clr, x, y + h / 2, clr, 0.0f);
}

static void draw_prev(float cx, float cy, u32 clr)
{
	tri_left(cx - 6.0f, cy - 8.0f, 12.0f, 16.0f, clr);
	C2D_DrawRectSolid(cx - 9.0f, cy - 8.0f, 0.0f, 3.0f, 16.0f, clr);
}

static void draw_next(float cx, float cy, u32 clr)
{
	tri_right(cx - 6.0f, cy - 8.0f, 12.0f, 16.0f, clr);
	C2D_DrawRectSolid(cx + 6.0f, cy - 8.0f, 0.0f, 3.0f, 16.0f, clr);
}

/* Two paths that cross in the middle and exit right as arrows, like Spotify's.
 *
 * Each path is a short horizontal run on the left, a diagonal through the
 * centre, then a short run out to its arrowhead - so the two strands visibly
 * swap sides, which is what makes it read as "shuffle" rather than as an X.
 * The earlier version ran the diagonals nearly parallel and lost that. */
static void draw_shuffle_glyph(float cx, float cy, u32 clr)
{
	const float t   = 2.0f;  /* stroke */
	const float x0  = cx - 10.0f;
	const float x1  = cx - 5.0f;   /* diagonal starts */
	const float x2  = cx + 4.0f;   /* diagonal ends */
	const float x3  = cx + 7.0f;   /* arrowhead base */
	const float dy  = 5.5f;

	/* upper-left -> lower-right */
	C2D_DrawRectSolid(x0, cy - dy - t / 2.0f, 0.0f, x1 - x0, t, clr);
	C2D_DrawLine(x1, cy - dy, clr, x2, cy + dy, clr, t, 0.0f);
	C2D_DrawRectSolid(x2, cy + dy - t / 2.0f, 0.0f, x3 - x2, t, clr);
	tri_right(x3, cy + dy - 4.0f, 5.5f, 8.0f, clr);

	/* lower-left -> upper-right */
	C2D_DrawRectSolid(x0, cy + dy - t / 2.0f, 0.0f, x1 - x0, t, clr);
	C2D_DrawLine(x1, cy + dy, clr, x2, cy - dy, clr, t, 0.0f);
	C2D_DrawRectSolid(x2, cy - dy - t / 2.0f, 0.0f, x3 - x2, t, clr);
	tri_right(x3, cy - dy - 4.0f, 5.5f, 8.0f, clr);
}

/* A closed loop of uniform stroke, with one arrowhead sitting on the top edge.
 *
 * The previous version tried to hang a small tail off the corner, which at this
 * size had too few pixels to read as anything and made the stroke look uneven.
 * Every segment is now exactly `t` thick and the corners overlap rather than
 * mitre, so the outline is continuous at any colour. */
static void draw_repeat_glyph(float cx, float cy, u32 clr)
{
	const float w = 17.0f, h = 13.0f, t = 2.0f;
	const float x = cx - w / 2.0f, y = cy - h / 2.0f;

	/* Rounded corners: inset each straight run by the radius, then lay a small
	 * disc of the same stroke width over each corner. Keeps the outline
	 * continuous and exactly `t` thick the whole way round. */
	const float r = 3.0f;

	C2D_DrawRectSolid(x + r, y, 0.0f, w - 2.0f * r, t, clr);          /* top */
	C2D_DrawRectSolid(x + r, y + h - t, 0.0f, w - 2.0f * r, t, clr);  /* bottom */
	C2D_DrawRectSolid(x, y + r, 0.0f, t, h - 2.0f * r, clr);          /* left */
	C2D_DrawRectSolid(x + w - t, y + r, 0.0f, t, h - 2.0f * r, clr);  /* right */

	/* Corner arcs, drawn as an outer disc with the interior punched back out
	 * would need a stencil; at this size a small filled disc centred on the
	 * stroke reads as a rounded corner and costs one draw each. */
	const float ci = t / 2.0f;
	ui_disc(x + r, y + ci, ci, clr);
	ui_disc(x + w - r, y + ci, ci, clr);
	ui_disc(x + ci, y + r, ci, clr);
	ui_disc(x + w - ci, y + r, ci, clr);
	ui_disc(x + r, y + h - ci, ci, clr);
	ui_disc(x + w - r, y + h - ci, ci, clr);
	ui_disc(x + ci, y + h - r, ci, clr);
	ui_disc(x + w - ci, y + h - r, ci, clr);

	/* Diagonal nubs bridging each corner gap, so the turn looks curved rather
	 * than notched. */
	C2D_DrawLine(x + ci, y + r, clr, x + r, y + ci, clr, t, 0.0f);
	C2D_DrawLine(x + w - ci, y + r, clr, x + w - r, y + ci, clr, t, 0.0f);
	C2D_DrawLine(x + ci, y + h - r, clr, x + r, y + h - ci, clr, t, 0.0f);
	C2D_DrawLine(x + w - ci, y + h - r, clr, x + w - r, y + h - ci, clr, t,
	             0.0f);

	/* Arrowhead riding on the top edge, pointing the way the loop travels.
	 * Sitting it on the stroke rather than off a corner gives it room. */
	tri_right(x + w - 9.0f, y - 3.0f, 6.0f, 8.0f, clr);
}

static void draw_playpause(float cx, float cy, bool playing, bool pressed)
{
	/* The disc is already the brightest thing on screen, so it presses by
	 * tinting rather than by gaining a halo it would hide anyway. */
	ui_disc(cx, cy, PLAY_R, pressed ? CLR_GREEN : CLR_WHITE);

	if (playing) {
		C2D_DrawRectSolid(cx - 6.0f, cy - 7.5f, 0.0f, 4.0f, 15.0f, CLR_DISC_FG);
		C2D_DrawRectSolid(cx + 2.0f, cy - 7.5f, 0.0f, 4.0f, 15.0f, CLR_DISC_FG);
	} else {
		/* Nudged right so the triangle looks centred in the disc. */
		tri_right(cx - 4.0f, cy - 8.0f, 14.0f, 16.0f, CLR_DISC_FG);
	}
}

/* Three bars plus ALL, the tile that opens the full list.
 *
 * Centred by measuring the group rather than by hardcoded offsets, so it stays
 * centred when the label size changes - which it already has twice. */
static void draw_all_tile(C2D_TextBuf buf, float x, float y, bool pressed)
{
	C2D_DrawRectSolid(x, y, 0.0f, TILE, TILE,
	                  pressed ? C2D_Color32(0x28, 0x28, 0x28, 0xFF) : CLR_TILE);

	const float bar_w = 18.0f, bar_h = 2.0f, bar_pitch = 5.0f;
	const float bars_h = bar_pitch * 2.0f + bar_h; /* 3 bars */
	const float gap    = 6.0f;
	const float text_h = ui_px(TY_MICRO);

	const float group_h = bars_h + gap + text_h;
	const float top     = y + (TILE - group_h) / 2.0f;

	float by = top;
	for (int i = 0; i < 3; i++) {
		C2D_DrawRectSolid(x + (TILE - bar_w) / 2.0f, by, 0.0f, bar_w, bar_h,
		                  CLR_BARS);
		by += bar_pitch;
	}

	const float tw = ui_text_width(buf, "ALL", TY_MICRO);
	ui_text(buf, "ALL", x + (TILE - tw) / 2.0f,
	        ui_baseline(top + bars_h + gap, TY_MICRO), TY_MICRO, TILE,
	        CLR_ALL_TXT);
}

static void fmt_time(long ms, char *out, int outlen)
{
	if (ms < 0)
		ms = 0;
	const long s = ms / 1000;
	snprintf(out, outlen, "%ld:%02ld", s / 60, s % 60);
}

void screen_player_draw(const screen_player_args *a)
{
	/* --- shelf -------------------------------------------------------- */
	ui_text_tracked(a->buf, "RECENTLY PLAYED", SHELF_LABEL_X,
	                ui_baseline(SHELF_LABEL_Y, TY_MICRO), TY_MICRO, 1.1f,
	                CLR_LABEL);

	for (int i = 0; i < SHELF_TILES; i++) {
		const float x = SHELF_X + (float)i * (TILE + TILE_GAP);
		if (a->shelf[i]) {
			const float s = TILE / (float)a->shelf[i]->subtex->width;
			C2D_DrawImageAt(*a->shelf[i], x, SHELF_Y, 0.0f, NULL, s, s);
		} else {
			C2D_DrawRectSolid(x, SHELF_Y, 0.0f, TILE, TILE, CLR_TILE_BG);
		}
		tb_add(a->tb, x, SHELF_Y, TILE, TILE, BTN_SHELF0 + i);
	}

	const float all_x = SHELF_X + (float)SHELF_TILES * (TILE + TILE_GAP);
	draw_all_tile(a->buf, all_x, SHELF_Y, a->pressed_id == BTN_SHELF_ALL);
	tb_add(a->tb, all_x, SHELF_Y, TILE, TILE, BTN_SHELF_ALL);

	/* --- transport ----------------------------------------------------- */
	/* Five controls on one baseline: shuffle prev PLAY next repeat. */
	const float span = CTRL_SMALL * 4.0f + PLAY_R * 2.0f + CTRL_GAP * 4.0f;
	float       cx   = (BOT_W - span) / 2.0f + CTRL_SMALL / 2.0f;

	const float shuf_x = cx;
	const float prev_x = cx + CTRL_SMALL + CTRL_GAP;
	const float play_x = prev_x + CTRL_SMALL / 2.0f + CTRL_GAP + PLAY_R;
	const float next_x = play_x + PLAY_R + CTRL_GAP + CTRL_SMALL / 2.0f;
	const float rep_x  = next_x + CTRL_SMALL + CTRL_GAP;

	/* Registered centre-outward: 44px hit rects on a 22px gap necessarily
	 * overlap, and touch_hit takes the first match, so the play button must be
	 * registered first to win the contested pixels. */
	tb_add_hit(a->tb, play_x, ROW_Y, PLAY_R * 2.0f, BTN_PLAY);
	tb_add_hit(a->tb, prev_x, ROW_Y, CTRL_SMALL, BTN_PREV);
	tb_add_hit(a->tb, next_x, ROW_Y, CTRL_SMALL, BTN_NEXT);
	tb_add_hit(a->tb, shuf_x, ROW_Y, CTRL_SMALL, BTN_SHUFFLE);
	tb_add_hit(a->tb, rep_x, ROW_Y, CTRL_SMALL, BTN_REPEAT);

	/* Shuffle and repeat sit outside prev/next and turn green with a dot when
	 * active, rather than changing shape. */
	/* Halos first, so every glyph draws on top of its own. */
	press_halo(shuf_x, ROW_Y, a->pressed_id == BTN_SHUFFLE);
	press_halo(prev_x, ROW_Y, a->pressed_id == BTN_PREV);
	press_halo(next_x, ROW_Y, a->pressed_id == BTN_NEXT);
	press_halo(rep_x, ROW_Y, a->pressed_id == BTN_REPEAT);

	const u32 shuf_clr = a->shuffle ? CLR_GREEN : CLR_IDLE;
	draw_shuffle_glyph(shuf_x, ROW_Y,
	                   press_clr(shuf_clr, a->pressed_id == BTN_SHUFFLE));
	if (a->shuffle)
		ui_disc(shuf_x, ROW_Y + 12.0f, 1.5f, CLR_GREEN);

	draw_prev(prev_x, ROW_Y, press_clr(CLR_WHITE, a->pressed_id == BTN_PREV));
	draw_playpause(play_x, ROW_Y, a->playing, a->pressed_id == BTN_PLAY);
	draw_next(next_x, ROW_Y, press_clr(CLR_WHITE, a->pressed_id == BTN_NEXT));

	const u32 rep_clr = a->repeat != REPEAT_OFF ? CLR_GREEN : CLR_IDLE;
	draw_repeat_glyph(rep_x, ROW_Y,
	                  press_clr(rep_clr, a->pressed_id == BTN_REPEAT));
	if (a->repeat != REPEAT_OFF) {
		ui_disc(rep_x, ROW_Y + 12.0f, 1.5f, CLR_GREEN);
		/* Repeat-one gets a second dot. A "1" glyph inside the loop would be
		 * the usual convention but is illegible at this size, and the state is
		 * shared with the user's other clients so it must stay visible. */
		if (a->repeat == REPEAT_TRACK)
			ui_disc(rep_x + 5.0f, ROW_Y + 12.0f, 1.5f, CLR_GREEN);
	}

	/* --- scrubber ------------------------------------------------------ */
	char elapsed[16], total[16];
	fmt_time(a->progress_ms, elapsed, sizeof elapsed);
	fmt_time(a->duration_ms, total, sizeof total);

	ui_text(a->buf, elapsed, SHELF_LABEL_X,
	        ui_baseline(SCRUB_BAR_Y - 4.0f, TY_ROW_SUB), TY_ROW_SUB, 30.0f,
	        CLR_TIME);

	const float tw = ui_text_width(a->buf, total, TY_ROW_SUB);
	ui_text(a->buf, total, BOT_W - SHELF_LABEL_X - tw,
	        ui_baseline(SCRUB_BAR_Y - 4.0f, TY_ROW_SUB), TY_ROW_SUB, 40.0f,
	        CLR_TIME);

	C2D_DrawRectSolid(SCRUB_BAR_X, SCRUB_BAR_Y, 0.0f, SCRUB_BAR_W, 4.0f,
	                  CLR_TRACK);

	if (a->duration_ms > 0) {
		float f = (float)a->progress_ms / (float)a->duration_ms;
		if (f < 0.0f)
			f = 0.0f;
		if (f > 1.0f)
			f = 1.0f;

		C2D_DrawRectSolid(SCRUB_BAR_X, SCRUB_BAR_Y, 0.0f, SCRUB_BAR_W * f, 4.0f,
		                  CLR_WHITE);
		ui_disc(SCRUB_BAR_X + SCRUB_BAR_W * f, SCRUB_BAR_Y + 2.0f,
		        a->scrubbing ? 7.0f : 5.0f, CLR_WHITE);
	}

	/* Generous strip so the 4px track is grabbable with a thumb. */
	tb_add(a->tb, SHELF_LABEL_X, SCRUB_BAR_Y - 18.0f, BOT_W - 32.0f, 40.0f,
	       BTN_SCRUB);
}
