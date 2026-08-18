#include "search_popover.h"

#include <math.h>
#include <stdio.h>

#include "ui.h"

#define BOT_W 320.0f
#define BOT_H 240.0f

/* Anchored to the search disc, which both headers draw at (302,15) with a
 * radius of 10 under a 30px header.
 *
 * The panel is right-aligned rather than centred on the button: 164px centred
 * on x=302 would run 64px off the screen. Its right edge sits at 314, level
 * with the scroll indicator column, and the notch still points at the button
 * from 12px inside that edge. */
#define PANEL_W   164.0f
#define PANEL_X   (314.0f - PANEL_W)
#define PANEL_Y   38.0f
#define NOTCH_CX  302.0f
#define NOTCH_HW  5.0f
#define NOTCH_TOP 30.0f
#define ROW_H     28.0f
#define HDR_H     22.0f
#define PAD_X     12.0f
/* Touch column for the per-row delete. Wide enough to hit deliberately even
 * though the mark drawn inside it is small. */
#define DEL_W     26.0f
/* The corner arc used to reach past the notch's base, so the notch appeared to
 * float over the curve rather than grow out of a straight edge. */
#define CORNER_R  4.0f

#define CLR_SCRIM   C2D_Color32(0x00, 0x00, 0x00, 0x98)
#define CLR_PANEL   C2D_Color32(0x1B, 0x1B, 0x1B, 0xFF)
#define CLR_BORDER  C2D_Color32(0x38, 0x38, 0x38, 0xFF)
#define CLR_SHADOW  C2D_Color32(0x00, 0x00, 0x00, 0x60)
#define CLR_HILITE  C2D_Color32(0x24, 0x24, 0x24, 0xFF)
#define CLR_DIVIDE  C2D_Color32(0x26, 0x26, 0x26, 0xFF)
#define CLR_FOOTDIV C2D_Color32(0x32, 0x32, 0x32, 0xFF)
#define CLR_NAME    C2D_Color32(0xE8, 0xE8, 0xE8, 0xFF)
#define CLR_CAPTION C2D_Color32(0x8A, 0x8A, 0x8A, 0xFF)
#define CLR_SUB     C2D_Color32(0x8A, 0x8A, 0x8A, 0xFF)
#define CLR_GREEN   C2D_Color32(0x1D, 0xB9, 0x54, 0xFF)
#define CLR_GREEN_PRESS C2D_Color32(0x28, 0xD8, 0x68, 0xFF)

static float panel_h(int count)
{
	return HDR_H + (float)count * ROW_H;
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

void search_popover_draw_ring(float cx, float cy, float progress)
{
	/* Nothing for the first fifth of the hold. Every ordinary tap passes
	 * through the early part of this range, and a stub of arc appearing under
	 * each one looks like a rendering fault rather than feedback. */
	if (progress <= 0.2f)
		return;
	if (progress > 1.0f)
		progress = 1.0f;
	const float swept = (progress - 0.2f) / 0.8f;

	/* Just outside the r=10 disc, and still inside the 30px header band, so
	 * there is nothing to clip against. */
	/* +2 points, not +1: the sweep is divided into n-1 steps, so a full
	 * circle needs SEGMENTS+2 of them. Sizing this off SEGMENTS+1 overran the
	 * array by one point at the very end of the hold. */
	enum { SEGMENTS = 24 };
	float      pts[(SEGMENTS + 2) * 2];
	int        n = (int)(swept * SEGMENTS) + 2;
	if (n > SEGMENTS + 2)
		n = SEGMENTS + 2;
	const float step = swept * 2.0f * (float)M_PI / (float)(n - 1);
	for (int i = 0; i < n; i++) {
		const float a = -(float)M_PI / 2.0f + (float)i * step;
		pts[i * 2] = cx + 13.0f * cosf(a);
		pts[i * 2 + 1] = cy + 13.0f * sinf(a);
	}
	ui_polyline(pts, n, 2.0f, CLR_GREEN);
}

void search_popover_add_hits(const search_popover_args *a)
{
	if (!a || !a->tb || a->count <= 0)
		return;

	for (int i = 0; i < a->count; i++) {
		const float top = PANEL_Y + HDR_H + (float)i * ROW_H;
		/* Delete first, so it wins the overlap: the row rect spans the whole
		 * width behind it and touch_hit takes whichever was added first. */
		tb_add(a->tb, PANEL_X + PANEL_W - DEL_W, top, DEL_W, ROW_H,
		       SEARCH_POP_DEL0 + i);
		tb_add(a->tb, PANEL_X, top, PANEL_W - DEL_W, ROW_H,
		       SEARCH_POP_ROW0 + i);
	}

	/* The header's right half; the full 22px of it, since the word itself is
	 * only 12px tall. */
	tb_add(a->tb, PANEL_X + PANEL_W / 2.0f, PANEL_Y, PANEL_W / 2.0f, HDR_H,
	       SEARCH_POP_CLEAR);

	/* Last, and covering everything: a tap on the panel matches one of the
	 * rects above first, and anything else falls through to here. Registered
	 * before the screen underneath adds its own, so it also swallows taps
	 * meant for the list - which is what being modal means. */
	tb_add(a->tb, 0.0f, 0.0f, BOT_W, BOT_H, SEARCH_POP_DISMISS);
}

void search_popover_draw(const search_popover_args *a)
{
	if (!a || a->count <= 0)
		return;
	const float h = panel_h(a->count);

	C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, BOT_W, BOT_H, CLR_SCRIM);
	rounded_rect(PANEL_X + 3.0f, PANEL_Y + 5.0f, PANEL_W, h, CORNER_R,
	             CLR_SHADOW);
	rounded_rect(PANEL_X, PANEL_Y, PANEL_W, h, CORNER_R, CLR_BORDER);
	rounded_rect(PANEL_X + 1.0f, PANEL_Y + 1.0f, PANEL_W - 2.0f, h - 2.0f,
	             CORNER_R - 1.0f, CLR_PANEL);

	/* Drawn after the panel so it covers the top border and the two read as
	 * one shape. A triangle, not a polyline: a stroke would outline it rather
	 * than fill it. */
	C2D_DrawTriangle(NOTCH_CX - NOTCH_HW - 1.0f, PANEL_Y + 1.0f, CLR_BORDER,
	                 NOTCH_CX + NOTCH_HW + 1.0f, PANEL_Y + 1.0f, CLR_BORDER,
	                 NOTCH_CX, NOTCH_TOP - 1.0f, CLR_BORDER, 0.0f);
	C2D_DrawTriangle(NOTCH_CX - NOTCH_HW, PANEL_Y + 1.0f, CLR_PANEL,
	                 NOTCH_CX + NOTCH_HW, PANEL_Y + 1.0f, CLR_PANEL, NOTCH_CX,
	                 NOTCH_TOP + 0.5f, CLR_PANEL, 0.0f);

	const float text_x = PANEL_X + PAD_X;
	const float text_w = PANEL_W - PAD_X - DEL_W;

	ui_text_tracked(a->buf, "RECENT", text_x,
	                ui_baseline(PANEL_Y + (HDR_H - ui_px(TY_MICRO)) / 2.0f,
	                            TY_MICRO),
	                TY_MICRO, 1.1f, CLR_CAPTION);

	/* Plain rather than tracked, because it is an action and not a caption -
	 * and because ui_text_width does not account for tracking, so a tracked
	 * string cannot be right-aligned accurately. */
	const bool clear_pressed = a->pressed_id == SEARCH_POP_CLEAR;
	const float clear_w = ui_text_width(a->buf, "CLEAR", TY_MICRO);
	ui_text(a->buf, "CLEAR", PANEL_X + PANEL_W - PAD_X - clear_w,
	        ui_baseline(PANEL_Y + (HDR_H - ui_px(TY_MICRO)) / 2.0f, TY_MICRO),
	        TY_MICRO, clear_w, clear_pressed ? CLR_GREEN_PRESS : CLR_GREEN);

	for (int i = 0; i < a->count; i++) {
		const float top = PANEL_Y + HDR_H + (float)i * ROW_H;
		const bool  pressed = a->pressed_id == SEARCH_POP_ROW0 + i;

		if (i == 0 || pressed)
			C2D_DrawRectSolid(PANEL_X + 1.0f, top, 0.0f, PANEL_W - 2.0f, ROW_H,
			                  CLR_HILITE);
		/* The green edge is what separates "most recent" from "being pressed",
		 * since both take the same fill. It is the same mark both lists use
		 * for the row currently playing. */
		if (i == 0)
			C2D_DrawRectSolid(PANEL_X + 1.0f, top, 0.0f, 3.0f, ROW_H,
			                  CLR_GREEN);
		if (i > 0)
			C2D_DrawRectSolid(PANEL_X + 1.0f, top, 0.0f, PANEL_W - 2.0f, 1.0f,
			                  CLR_DIVIDE);

		if (a->queries[i])
			ui_text(a->buf, a->queries[i], text_x,
			        ui_baseline(top + (ROW_H - ui_px(TY_ROW_NAME)) / 2.0f,
			                    TY_ROW_NAME),
			        TY_ROW_NAME, text_w, CLR_NAME);

		/* Forget just this one. Drawn small and quiet so it does not compete
		 * with the query, but given a full-height column to hit: the mark is
		 * the target's centre, not its extent. */
		const bool del_pressed = a->pressed_id == SEARCH_POP_DEL0 + i;
		const float dx = PANEL_X + PANEL_W - DEL_W / 2.0f;
		const float dy = top + ROW_H / 2.0f;
		const u32 dclr = del_pressed ? CLR_GREEN_PRESS : CLR_CAPTION;
		const float down[4] = {dx - 3.5f, dy - 3.5f, dx + 3.5f, dy + 3.5f};
		const float up[4] = {dx + 3.5f, dy - 3.5f, dx - 3.5f, dy + 3.5f};
		ui_polyline(down, 2, 1.6f, dclr);
		ui_polyline(up, 2, 1.6f, dclr);
	}

}
