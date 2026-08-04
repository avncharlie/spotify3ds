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
static void draw_repeat_glyph(float cx, float cy, u32 clr, bool one)
{
	const float w = 17.0f, h = 13.0f, t = 2.0f;
	const float x = cx - w / 2.0f, y = cy - h / 2.0f;
	const float r = 3.5f; /* corner radius, measured to the stroke centre */

	/* The loop opens on the bottom-left. The arrowhead points left - the
	 * direction the loop travels - with its flat base joined to the bottom run
	 * and the gap after its tip, so the head terminates the stroke rather than
	 * floating detached from it. */
	const float gap    = 2.0f;
	const float head_w = 5.5f;

	/* Straight runs, inset by the radius at both ends. In repeat-one the top run
	 * is broken by a notch and the digit sits astride the break, which is how
	 * Spotify draws it - a "1" laid over an unbroken bar reads as struck
	 * through. */
	const float digit_cx = cx + 1.0f; /* the flag hangs left, so nudging the stem
	                                   * right centres the digit optically */

	if (one) {
		/* Asymmetric on purpose, and the two clearances differ as well. The
		 * flag is a diagonal, so only its lowest point reaches ink_l; level
		 * with the bar its ink sits further right, which makes an equal
		 * clearance look narrower on the left than on the right. The extra
		 * pixel is measured off the screen, not derived. */
		const float ink_l   = digit_cx - 3.0f; /* flag tip  */
		const float ink_r   = digit_cx + 1.0f; /* stem edge */
		const float notch_l = ink_l - 2.5f;
		const float notch_r = ink_r + 1.5f;

		C2D_DrawRectSolid(x + r, y, 0.0f, notch_l - (x + r), t, clr);
		C2D_DrawRectSolid(notch_r, y, 0.0f, x + w - r - notch_r, t, clr);
	} else {
		C2D_DrawRectSolid(x + r, y, 0.0f, w - 2.0f * r, t, clr);  /* top */
	}

	/* Bottom run starts past the arrowhead, which sits inboard of the
	 * bottom-left corner rather than replacing it. */
	const float bottom_x = x + r + head_w + gap;
	C2D_DrawRectSolid(bottom_x, y + h - t, 0.0f, x + w - r - bottom_x, t, clr);

	C2D_DrawRectSolid(x, y + r, 0.0f, t, h - 2.0f * r, clr);         /* left */
	C2D_DrawRectSolid(x + w - t, y + r, 0.0f, t, h - 2.0f * r, clr); /* right */

	/* Corners as short arcs of line segments. An earlier attempt laid a
	 * one-pixel disc on each corner, which is too small to round anything -
	 * hence the square top-left. Stepping round the quarter turn and stroking
	 * between the points actually curves it. */
	const float m  = t / 2.0f;
	const float rr = r - m; /* radius of the stroke centreline */

	/* All four corners are drawn. The opening for the arrow sits along the
	 * bottom edge past the corner, not at it. */
	struct { float ox, oy, a0; } corners[4] = {
		{x + r,     y + r,     180.0f}, /* top-left */
		{x + w - r, y + r,     270.0f}, /* top-right */
		{x + w - r, y + h - r,   0.0f}, /* bottom-right */
		{x + r,     y + h - r,  90.0f}, /* bottom-left */
	};

	for (int c = 0; c < 4; c++) {
		const int steps = 4;
		float     px = 0.0f, py = 0.0f;
		for (int i = 0; i <= steps; i++) {
			const float a = (corners[c].a0 + 90.0f * (float)i / (float)steps) *
			                (float)M_PI / 180.0f;
			const float nx = corners[c].ox + rr * cosf(a);
			const float ny = corners[c].oy + rr * sinf(a);
			if (i)
				C2D_DrawLine(px, py, clr, nx, ny, clr, t, 0.0f);
			px = nx;
			py = ny;
		}
	}

	/* Arrowhead pointing left, its flat base flush against the start of the
	 * bottom run so the stroke reads as continuing into the head. The gap is
	 * beyond the tip. */
	tri_left(bottom_x - head_w, y + h - t / 2.0f - 4.0f, head_w, 8.0f, clr);

	/* The "1" straddles the notch in the top run: the system font has nothing
	 * legible at this size, so it is stroked by hand as a stem plus a short
	 * flag. Centred on the top edge rather than on the loop, so the notch reads
	 * as the digit displacing the bar. */
	if (one) {
		const float dt  = 2.0f;
		const float top = y - 2.0f;
		const float sx  = digit_cx - dt / 2.0f;

		C2D_DrawRectSolid(sx, top, 0.0f, dt, 8.0f, clr);
		C2D_DrawLine(sx, top + 2.0f, clr, sx - 2.0f, top + 3.5f, clr, dt, 0.0f);
	}
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
	                  press_clr(rep_clr, a->pressed_id == BTN_REPEAT),
	                  a->repeat == REPEAT_TRACK);
	/* One dot for either active state; repeat-one is told apart by the "1"
	 * inside the loop, as Spotify does it. */
	if (a->repeat != REPEAT_OFF)
		ui_disc(rep_x, ROW_Y + 12.0f, 1.5f, CLR_GREEN);

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
