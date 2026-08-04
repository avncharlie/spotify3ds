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

/* A pressed control dims slightly. With the boxes gone there is no fill to
 * highlight, so the glyph itself has to carry the feedback. */
static u32 dim_if(u32 clr, bool pressed)
{
	if (!pressed)
		return clr;
	const u8 r = (clr >> 0) & 0xFF, g = (clr >> 8) & 0xFF, b = (clr >> 16) & 0xFF;
	return C2D_Color32((u8)(r * 6 / 10), (u8)(g * 6 / 10), (u8)(b * 6 / 10),
	                   0xFF);
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

/* Two crossing arrows. At this size the crossing reads better as two straight
 * runs than as the mockup's curved SVG paths, which would need many segments
 * to avoid looking ragged. */
static void draw_shuffle_glyph(float cx, float cy, u32 clr)
{
	C2D_DrawLine(cx - 8.5f, cy - 4.0f, clr, cx + 4.0f, cy + 4.0f, clr, 1.8f,
	             0.0f);
	C2D_DrawLine(cx - 8.5f, cy + 4.0f, clr, cx + 4.0f, cy - 4.0f, clr, 1.8f,
	             0.0f);
	tri_right(cx + 3.0f, cy - 7.5f, 4.5f, 7.0f, clr);
	tri_right(cx + 3.0f, cy + 0.5f, 4.5f, 7.0f, clr);
}

/* Rounded-rectangle loop with a return arrow. */
static void draw_repeat_glyph(float cx, float cy, u32 clr)
{
	const float w = 15.0f, h = 12.0f, t = 1.8f;
	const float x = cx - w / 2.0f, y = cy - h / 2.0f;

	C2D_DrawRectSolid(x + 2.0f, y, 0.0f, w - 4.0f, t, clr);          /* top */
	C2D_DrawRectSolid(x + 2.0f, y + h - t, 0.0f, w - 4.0f, t, clr);  /* bottom */
	C2D_DrawRectSolid(x, y + 2.0f, 0.0f, t, h - 4.0f, clr);          /* left */
	C2D_DrawRectSolid(x + w - t, y + 2.0f, 0.0f, t, h - 4.0f, clr);  /* right */

	/* Arrowhead where the loop restarts. */
	tri_left(x + 1.0f, y - 2.5f, 5.0f, 6.0f, clr);
}

static void draw_playpause(float cx, float cy, bool playing)
{
	ui_disc(cx, cy, PLAY_R, CLR_WHITE);

	if (playing) {
		C2D_DrawRectSolid(cx - 6.0f, cy - 7.5f, 0.0f, 4.0f, 15.0f, CLR_DISC_FG);
		C2D_DrawRectSolid(cx + 2.0f, cy - 7.5f, 0.0f, 4.0f, 15.0f, CLR_DISC_FG);
	} else {
		/* Nudged right so the triangle looks centred in the disc. */
		tri_right(cx - 4.0f, cy - 8.0f, 14.0f, 16.0f, CLR_DISC_FG);
	}
}

/* Three bars plus ALL, the tile that opens the full list. */
static void draw_all_tile(C2D_TextBuf buf, float x, float y, bool pressed)
{
	C2D_DrawRectSolid(x, y, 0.0f, TILE, TILE,
	                  pressed ? C2D_Color32(0x28, 0x28, 0x28, 0xFF) : CLR_TILE);

	const float bx = x + (TILE - 18.0f) / 2.0f;
	float       by = y + 14.0f;
	for (int i = 0; i < 3; i++) {
		C2D_DrawRectSolid(bx, by, 0.0f, 18.0f, 2.0f, CLR_BARS);
		by += 5.0f;
	}

	const float tw = ui_text_width(buf, "ALL", TY_MICRO);
	ui_text(buf, "ALL", x + (TILE - tw) / 2.0f,
	        ui_baseline(y + TILE - 16.0f, TY_MICRO), TY_MICRO, TILE,
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
	const u32 shuf_clr = a->shuffle ? CLR_GREEN : CLR_IDLE;
	draw_shuffle_glyph(shuf_x, ROW_Y,
	                   dim_if(shuf_clr, a->pressed_id == BTN_SHUFFLE));
	if (a->shuffle)
		ui_disc(shuf_x, ROW_Y + 12.0f, 1.5f, CLR_GREEN);

	draw_prev(prev_x, ROW_Y, dim_if(CLR_WHITE, a->pressed_id == BTN_PREV));
	draw_playpause(play_x, ROW_Y, a->playing);
	draw_next(next_x, ROW_Y, dim_if(CLR_WHITE, a->pressed_id == BTN_NEXT));

	const u32 rep_clr = a->repeat != REPEAT_OFF ? CLR_GREEN : CLR_IDLE;
	draw_repeat_glyph(rep_x, ROW_Y,
	                  dim_if(rep_clr, a->pressed_id == BTN_REPEAT));
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
