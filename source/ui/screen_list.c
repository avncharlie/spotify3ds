#include "screen_list.h"

#include <stdio.h>

#include "ui.h"

#define BOT_W 320.0f
#define BOT_H 240.0f

#define PAD_X    16.0f
#define THUMB    32.0f
#define THUMB_GAP 10.0f

/* Scroll indicator, per the mockup: a thin track down the right edge. */
#define IND_X 314.0f
#define IND_W 3.0f
#define IND_Y 40.0f
#define IND_H 130.0f

#define CLR_HEADER   C2D_Color32(0x11, 0x11, 0x11, 0xFF)
#define CLR_ROW_SEL  C2D_Color32(0x17, 0x17, 0x17, 0xFF)
#define CLR_ROW_PRESS C2D_Color32(0x24, 0x24, 0x24, 0xFF)
#define CLR_NAME     C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define CLR_SUB      C2D_Color32(0x8A, 0x8A, 0x8A, 0xFF)
#define CLR_THUMB_BG C2D_Color32(0x22, 0x22, 0x2A, 0xFF)
#define CLR_IND_TRK  C2D_Color32(0x33, 0x33, 0x33, 0xFF)
#define CLR_IND_THMB C2D_Color32(0x7A, 0x7A, 0x7A, 0xFF)

/* Visible height below the header. */
static float viewport_h(void)
{
	return BOT_H - LIST_HEADER_H;
}

float screen_list_max_scroll(int count)
{
	const float content = (float)count * LIST_ROW_H;
	const float max     = content - viewport_h();
	return max > 0.0f ? max : 0.0f;
}

void screen_list_draw(const screen_list_args *a)
{
	const int n = a->items ? a->items->count : 0;

	/* --- rows ---------------------------------------------------------- */
	/* Drawn first, then the header is painted over the top: citro2d has no
	 * scissor, so a row scrolled up under the header would otherwise show
	 * through it. */
	for (int i = 0; i < n; i++) {
		const float y = LIST_HEADER_H + (float)i * LIST_ROW_H - a->scroll;

		if (y > BOT_H || y + LIST_ROW_H < 0.0f)
			continue; /* off screen */

		const bool pressed = a->pressed_id == LIST_ROW0 + i;
		if (pressed)
			C2D_DrawRectSolid(0.0f, y, 0.0f, BOT_W, LIST_ROW_H, CLR_ROW_PRESS);
		else if (i == 0)
			C2D_DrawRectSolid(0.0f, y, 0.0f, BOT_W, LIST_ROW_H, CLR_ROW_SEL);

		const float ty = y + (LIST_ROW_H - THUMB) / 2.0f;
		if (a->art && a->art[i]) {
			const float s = THUMB / (float)a->art[i]->subtex->width;
			C2D_DrawImageAt(*a->art[i], PAD_X, ty, 0.0f, NULL, s, s);
		} else {
			C2D_DrawRectSolid(PAD_X, ty, 0.0f, THUMB, THUMB, CLR_THUMB_BG);
		}

		const float tx = PAD_X + THUMB + THUMB_GAP;
		const float tw = BOT_W - tx - PAD_X;

		/* Two lines centred as a group on the row. */
		const float name_h = ui_px(TY_ROW_NAME);
		const float sub_h  = ui_px(TY_ROW_SUB);
		const float gap    = 3.0f;
		const float top    = y + (LIST_ROW_H - (name_h + gap + sub_h)) / 2.0f;

		ui_text(a->buf, a->items->items[i].name, tx,
		        ui_baseline(top, TY_ROW_NAME), TY_ROW_NAME, tw, CLR_NAME);
		ui_text(a->buf, a->items->items[i].subtitle, tx,
		        ui_baseline(top + name_h + gap, TY_ROW_SUB), TY_ROW_SUB, tw,
		        CLR_SUB);

		/* Only the part below the header is tappable, so a row peeking under
		 * it cannot be hit through the header. */
		const float hit_y = y < LIST_HEADER_H ? LIST_HEADER_H : y;
		const float hit_h = y + LIST_ROW_H - hit_y;
		if (hit_h > 8.0f)
			tb_add(a->tb, 0.0f, hit_y, BOT_W, hit_h, LIST_ROW0 + i);
	}

	/* --- scroll indicator ---------------------------------------------- */
	const float max_scroll = screen_list_max_scroll(n);
	if (max_scroll > 0.0f) {
		C2D_DrawRectSolid(IND_X, IND_Y, 0.0f, IND_W, IND_H, CLR_IND_TRK);

		/* Sized from the real item count rather than the mockup's fixed 68px,
		 * which was drawn for one particular list length. */
		float th = IND_H * viewport_h() / ((float)n * LIST_ROW_H);
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

	ui_text(a->buf, "Recently played", ax + 17.0f,
	        ui_baseline(LIST_HEADER_H / 2.0f - ui_px(TY_ROW_NAME) / 2.0f,
	                    TY_ROW_NAME),
	        TY_ROW_NAME, 240.0f, CLR_NAME);

	/* Generous hit area: the arrow itself is small. */
	tb_add(a->tb, 0.0f, 0.0f, 90.0f, LIST_HEADER_H, LIST_BTN_BACK);
}
