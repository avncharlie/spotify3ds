#include "ui.h"

#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../testlog.h"

/* Measured identical on the emulator and on a New 2DS XL (firmware
 * 11.17.0-50E): height=30, ascent=25, lineFeed=30, and every derived width
 * matching to the decimal. So typography can be judged in Azahar without a
 * hardware round trip - unlike most things in this project, the font is not a
 * divergence. Still read from fontGetInfo rather than hardcoded, since other
 * regions were not measured. */

/* The mockup's CSS px per role. TY_MICRO is deliberately absent: it is
 * variable while the 7px-vs-legible question is being settled. */
/* Artist and album run 2px above the mockup (14/12 rather than 12/10, and
 * 19/15 rather than 17/13 in the art-off layout). At the mockup's sizes they
 * read as too quiet against the title on the actual panel - the gaps were
 * right, the weight was not, and there is no bold to lean on here. */
static const float s_role_px[TY_COUNT] = {
	[TY_TITLE_L] = 34.0f, [TY_TITLE] = 21.0f,  [TY_ARTIST_L] = 19.0f,
	[TY_ALBUM_L] = 15.0f, [TY_ARTIST] = 14.0f, [TY_ROW_NAME] = 11.0f,
	[TY_ALBUM]   = 12.0f, [TY_ROW_SUB] = 8.0f, [TY_MICRO] = 7.0f,
};

static float s_scale[TY_COUNT];

/* The mockup asks for 7px here. Rendered and compared side by side: at 7px the
 * label is an unreadable smudge (the glyph atlas is minified ~4x and the
 * bilinear filter finishes it off), while at 10px "ALVBOOK" reads cleanly and
 * still carries the tracking that gives the mockup its character. 10px it is;
 * ui_set_micro_px keeps the choice adjustable. */
static float s_micro_px = 10.0f;
static float s_em;              /* font em box at scale 1.0 */
static float s_ascent;

void ui_init(void)
{
	FINF_s *fi = fontGetInfo(NULL);

	/* CSS font-size is the em box; the font's `height` is its em box at
	 * scale 1.0. Guard against a zero so a surprising font cannot divide by
	 * zero and blank every string on screen. */
	s_em     = fi && fi->height ? (float)fi->height : 30.0f;
	s_ascent = fi ? (float)fi->ascent : 25.0f;

	for (int i = 0; i < TY_COUNT; i++)
		s_scale[i] = s_role_px[i] / s_em;
}

float ui_scale(type_role r)
{
	if (r == TY_MICRO)
		return s_micro_px / s_em;
	return s_scale[r];
}

float ui_px(type_role r)
{
	return r == TY_MICRO ? s_micro_px : s_role_px[r];
}

float ui_micro_px(void)
{
	return s_micro_px;
}

void ui_set_micro_px(float px)
{
	s_micro_px = px;
}

float ui_baseline(float top, type_role r)
{
	return top + s_ascent * ui_scale(r);
}

float ui_text_width(C2D_TextBuf buf, const char *s, type_role r)
{
	if (!s || !s[0])
		return 0.0f;

	C2D_Text t;
	C2D_TextParse(&t, buf, s);
	C2D_TextOptimize(&t);

	const float sc = ui_scale(r);
	float       w = 0.0f, h = 0.0f;
	C2D_TextGetDimensions(&t, sc, sc, &w, &h);
	return w;
}

void ui_text(C2D_TextBuf buf, const char *s, float x, float y, type_role r,
             float maxw, u32 clr)
{
	if (!s || !s[0])
		return;

	char tmp[256];
	snprintf(tmp, sizeof tmp, "%s", s);

	const float sc = ui_scale(r);

	C2D_Text t;
	C2D_TextParse(&t, buf, tmp);
	C2D_TextOptimize(&t);

	float w = 0.0f, h = 0.0f;
	C2D_TextGetDimensions(&t, sc, sc, &w, &h);

	/* Trim a character at a time rather than assuming a glyph width: the
	 * system font is proportional. */
	int len = (int)strlen(tmp);
	while (maxw > 0.0f && w > maxw && len > 2) {
		len--;
		tmp[len - 1] = '.';
		tmp[len]     = '.';
		tmp[len + 1] = '\0';
		C2D_TextParse(&t, buf, tmp);
		C2D_TextOptimize(&t);
		C2D_TextGetDimensions(&t, sc, sc, &w, &h);
	}

	C2D_DrawText(&t, C2D_WithColor | C2D_AtBaseline, x, y, 0.0f, sc, sc, clr);
}

void ui_text_tracked(C2D_TextBuf buf, const char *s, float x, float y,
                     type_role r, float tracking_px, u32 clr)
{
	if (!s || !s[0])
		return;

	const float sc = ui_scale(r);
	float       pen = x;

	for (const char *p = s; *p; p++) {
		const char one[2] = {*p, '\0'};

		C2D_Text t;
		C2D_TextParse(&t, buf, one);
		C2D_TextOptimize(&t);

		float w = 0.0f, h = 0.0f;
		C2D_TextGetDimensions(&t, sc, sc, &w, &h);

		/* Spaces measure narrow in this font; nudge them so tracked labels
		 * keep their word gaps. */
		if (*p != ' ')
			C2D_DrawText(&t, C2D_WithColor | C2D_AtBaseline, pen, y, 0.0f, sc,
			             sc, clr);

		pen += w + tracking_px;
	}
}

void ui_disc(float cx, float cy, float r, u32 clr)
{
	const int segs = r >= 12.0f ? 16 : 10;

	float px = cx + r, py = cy;
	for (int i = 1; i <= segs; i++) {
		const float a  = (float)i * 2.0f * (float)M_PI / (float)segs;
		const float nx = cx + r * cosf(a);
		const float ny = cy + r * sinf(a);
		C2D_DrawTriangle(cx, cy, clr, px, py, clr, nx, ny, clr, 0.0f);
		px = nx;
		py = ny;
	}
}

void ui_font_probe(void)
{
	FINF_s *fi = fontGetInfo(NULL);
	if (!fi) {
		tl_timing("font: fontGetInfo returned NULL");
		return;
	}

	tl_timing("font: height=%u ascent=%u descent=%u lineFeed=%u alterCharIndex=%u",
	          (unsigned)fi->height, (unsigned)fi->ascent,
	          (unsigned)(fi->height - fi->ascent), (unsigned)fi->lineFeed,
	          (unsigned)fi->alterCharIndex);

	C2D_TextBuf buf = C2D_TextBufNew(256);

	/* Report, for each role, the derived scale and what a real string actually
	 * measures. The line box is taller than the em, so measured_h > px is
	 * expected - that gap is exactly why layout stacks by baseline. */
	static const char *sample = "Sample Track";
	for (int i = 0; i < TY_COUNT; i++) {
		const float sc = ui_scale((type_role)i);

		C2D_Text t;
		C2D_TextBufClear(buf);
		C2D_TextParse(&t, buf, sample);
		C2D_TextOptimize(&t);

		float w = 0.0f, h = 0.0f;
		C2D_TextGetDimensions(&t, sc, sc, &w, &h);

		tl_timing("font: role=%d px=%.0f scale=%.4f measured_w=%.1f "
		          "linebox_h=%.1f baseline_off=%.1f",
		          i, ui_px((type_role)i), sc, w, h, s_ascent * sc);
	}

	/* Cap height for the micro label at both candidate sizes, since that is
	 * the decision this probe exists to inform. */
	for (float px = 7.0f; px <= 10.5f; px += 1.0f) {
		const float sc = px / s_em;

		C2D_Text t;
		C2D_TextBufClear(buf);
		C2D_TextParse(&t, buf, "RECENTLY PLAYED");
		C2D_TextOptimize(&t);

		float w = 0.0f, h = 0.0f;
		C2D_TextGetDimensions(&t, sc, sc, &w, &h);
		tl_timing("font: micro px=%.0f scale=%.4f label_w=%.1f (fits 288? %s)",
		          px, sc, w, w <= 288.0f ? "yes" : "NO");
	}

	C2D_TextBufDelete(buf);
}
