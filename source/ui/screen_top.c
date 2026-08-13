#include "screen_top.h"

#include <stdio.h>
#include <string.h>

#include "ui.h"

/* Layout, straight from the mockup (1A: art on, 2A: art hidden). */
#define TOP_W 400.0f
#define TOP_H 240.0f

#define ART_X 16.0f
#define ART_Y 20.0f
#define ART_D 200.0f

/* 1A text column */
#define COL_X   232.0f
#define COL_TOP 46.0f
#define COL_W   152.0f

/* 2A text block */
#define BIG_X 40.0f
#define BIG_W 320.0f

/* Fatal-error block. Wider than either playback layout and starting higher,
 * because it has three wrapped paragraphs to fit rather than three short
 * lines - the top screen is 400x240. */
#define ERR_X   28.0f
#define ERR_TOP 34.0f
#define ERR_W   344.0f

/* Gaps between baselines. Scaled up with the type, which runs ~17% above the
 * mockup's sizes (see s_role_px): holding the mockup's gaps while enlarging the
 * text would have tightened the block relative to what it specified. */
#define GAP_1A_ARTIST 12.0f
#define GAP_1A_ALBUM  5.0f
#define GAP_1A_DEVICE 26.0f

#define GAP_2A_ARTIST 18.0f
#define GAP_2A_ALBUM  7.0f
#define GAP_2A_DEVICE 29.0f

#define CLR_TITLE  C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define CLR_ARTIST C2D_Color32(0xD2, 0xC8, 0xCC, 0xFF)
#define CLR_ALBUM  C2D_Color32(0x9A, 0x8C, 0x92, 0xFF)
#define CLR_DEVICE C2D_Color32(0xA8, 0x9A, 0xA0, 0xFF)
#define CLR_GREEN  C2D_Color32(0x1D, 0xB9, 0x54, 0xFF)
#define CLR_ERROR  C2D_Color32(0xFF, 0x6B, 0x5B, 0xFF)
#define CLR_ERR_D  C2D_Color32(0xC0, 0x55, 0x4A, 0xFF)
#define CLR_DIM    C2D_Color32(0xB0, 0xB0, 0xB0, 0xFF)
#define CLR_FAINT  C2D_Color32(0x70, 0x70, 0x70, 0xFF)

/* Tracking on the micro label, in px at its own size. */
#define DEVICE_TRACKING 0.6f

/* The mockup's wash is sampled from one particular cover, so only the *shape*
 * of its ramp is reusable - the hue has to come from the current art or every
 * album would be maroon. These are the mockup's own stop colours expressed as
 * ratios of its first stop. */
static void draw_wash(u8 ar, u8 ag, u8 ab)
{
	static const struct {
		float pos;   /* fraction of screen height */
		float ratio; /* brightness relative to the accent */
	} stops[] = {
		{0.00f, 1.000f},
		{0.46f, 0.935f},
		{0.78f, 0.467f},
		{1.00f, 0.000f},
	};

	for (int i = 0; i + 1 < (int)(sizeof stops / sizeof stops[0]); i++) {
		const float y0 = stops[i].pos * TOP_H;
		const float y1 = stops[i + 1].pos * TOP_H;

		const u32 c0 = C2D_Color32((u8)(ar * stops[i].ratio),
		                           (u8)(ag * stops[i].ratio),
		                           (u8)(ab * stops[i].ratio), 0xFF);
		const u32 c1 = C2D_Color32((u8)(ar * stops[i + 1].ratio),
		                           (u8)(ag * stops[i + 1].ratio),
		                           (u8)(ab * stops[i + 1].ratio), 0xFF);

		/* Four-corner colours give true vertex interpolation, so three rects
		 * replace the eight alpha bands this used to stack. */
		C2D_DrawRectangle(0.0f, y0, 0.0f, TOP_W, y1 - y0, c0, c0, c1, c1);
	}
}

/* Green dot plus the device name, tracked. Returns nothing; the caller has
 * already decided the baseline. */
static void draw_device(C2D_TextBuf buf, const char *name, float x,
                        float baseline)
{
	if (!name || !name[0])
		return;

	/* Dot sits on the text's optical centre rather than its baseline. */
	const float dot_r = 2.5f;
	const float dot_y = baseline - ui_px(TY_MICRO) * 0.35f;
	ui_disc(x + dot_r, dot_y, dot_r, CLR_GREEN);

	char upper[64];
	int  i = 0;
	for (; name[i] && i < (int)sizeof upper - 1; i++)
		upper[i] = (name[i] >= 'a' && name[i] <= 'z') ? (char)(name[i] - 32)
		                                              : name[i];
	upper[i] = '\0';

	ui_text_tracked(buf, upper, x + dot_r * 2.0f + 5.0f, baseline, TY_MICRO,
	                DEVICE_TRACKING, CLR_DEVICE);
}

void screen_top_draw(const screen_top_args *a)
{
	const bool show_art = a->art && a->art->valid && !a->art_hidden;

	/* Keep the wash even with art hidden: it is the last cover's colour, and
	 * losing it would make the art-off layout look like a different app. */
	if (a->art && a->art->valid)
		draw_wash(a->art->accent_r, a->art->accent_g, a->art->accent_b);
	else
		draw_wash(0x2A, 0x2A, 0x30);

	if (show_art) {
		const float sx = ART_D / (float)a->art->sub.width;
		const float sy = ART_D / (float)a->art->sub.height;
		C2D_DrawImageAt(a->art->image, ART_X, ART_Y, 0.0f, NULL, sx, sy);
	}

	/* --- a setup problem the user must fix ----------------------------
	 *
	 * Errors are prose, not a song name: they have to be read to the end. So
	 * this ignores the now-playing type scale - whose 34px title ellipsised
	 * "Spotify rejected the l.." and cut the actionable half of the hint - and
	 * wraps across the full screen width instead. The cover is irrelevant while
	 * the app is unusable, so the message always gets the whole screen. */
	if (a->fatal) {
		float bl = ui_baseline(ERR_TOP, TY_ARTIST_L);

		bl = ui_text_wrapped(a->buf, a->status, ERR_X, bl, TY_ARTIST_L, ERR_W,
		                     CLR_ERROR, 2, 1.25f);

		if (a->hint && a->hint[0]) {
			bl += ui_px(TY_ARTIST_L) * 0.5f + ui_px(TY_ALBUM_L);
			bl = ui_text_wrapped(a->buf, a->hint, ERR_X, bl, TY_ALBUM_L, ERR_W,
			                     CLR_ERR_D, 3, 1.35f);
		}

		/* The raw error, dimmed: it is for transcribing into a bug report,
		 * while the hint above is what the user is meant to act on. */
		if (a->detail && a->detail[0]) {
			bl += ui_px(TY_ALBUM_L) * 0.5f + ui_px(TY_ROW_SUB);
			ui_text_wrapped(a->buf, a->detail, ERR_X, bl, TY_ROW_SUB, ERR_W,
			                CLR_FAINT, 2, 1.3f);
		}
		return;
	}

	/* --- no state: say so, and say what to do about it ---------------- */
	if (!a->have_state) {
		const float x = show_art ? COL_X : BIG_X;
		const float w = show_art ? COL_W : BIG_W;
		const type_role role = show_art ? TY_TITLE : TY_TITLE_L;

		const char *primary = a->status[0] ? a->status : "Nothing playing";

		float bl = ui_baseline(show_art ? COL_TOP : 96.0f, role);
		ui_text(a->buf, primary, x, bl, role, w, CLR_DIM);

		if (a->hint && a->hint[0]) {
			bl += ui_px(role) * 0.6f + ui_px(TY_ALBUM_L);
			ui_text(a->buf, a->hint, x, bl, TY_ALBUM_L, w, CLR_FAINT);
		}
		return;
	}

	/* --- 1A: beside the cover ----------------------------------------- */
	if (show_art) {
		float bl = ui_baseline(COL_TOP, TY_TITLE);
		ui_text(a->buf, a->track, COL_X, bl, TY_TITLE, COL_W, CLR_TITLE);

		bl += GAP_1A_ARTIST + ui_px(TY_ARTIST);
		ui_text(a->buf, a->artist, COL_X, bl, TY_ARTIST, COL_W, CLR_ARTIST);

		bl += GAP_1A_ALBUM + ui_px(TY_ALBUM);
		ui_text(a->buf, a->album, COL_X, bl, TY_ALBUM, COL_W, CLR_ALBUM);

		bl += GAP_1A_DEVICE + ui_px(TY_MICRO);
		draw_device(a->buf, a->device, COL_X, bl);
		return;
	}

	/* --- 2A: art hidden, one block owning the screen ------------------- */
	/* Centre the block vertically: sum the heights and gaps, then start high
	 * enough that the whole group sits mid-screen. */
	const float block_h = ui_px(TY_TITLE_L) + GAP_2A_ARTIST +
	                      ui_px(TY_ARTIST_L) + GAP_2A_ALBUM +
	                      ui_px(TY_ALBUM_L) + GAP_2A_DEVICE + ui_px(TY_MICRO);
	const float top = (TOP_H - block_h) * 0.5f;

	float bl = ui_baseline(top, TY_TITLE_L);
	ui_text(a->buf, a->track, BIG_X, bl, TY_TITLE_L, BIG_W, CLR_TITLE);

	bl += GAP_2A_ARTIST + ui_px(TY_ARTIST_L);
	ui_text(a->buf, a->artist, BIG_X, bl, TY_ARTIST_L, BIG_W, CLR_ARTIST);

	bl += GAP_2A_ALBUM + ui_px(TY_ALBUM_L);
	ui_text(a->buf, a->album, BIG_X, bl, TY_ALBUM_L, BIG_W, CLR_ALBUM);

	bl += GAP_2A_DEVICE + ui_px(TY_MICRO);
	draw_device(a->buf, a->device, BIG_X, bl);
}
