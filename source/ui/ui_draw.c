#include "ui.h"

#include <3ds.h>
#include <ctype.h>
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
	[TY_ALBUM_L] = 15.0f, [TY_ARTIST] = 14.0f, [TY_ROW_NAME] = 14.0f,
	[TY_ALBUM]   = 12.0f, [TY_ROW_SUB] = 12.0f, [TY_MICRO] = 7.0f,
};
/* TY_ROW_SUB carries the scrubber times and the list-row subtitles. The mockup
 * puts it at 8px, which lands under the same legibility floor that forced the
 * micro label up: rendered, "1:23" was an unreadable smudge. 12px reads
 * comfortably at arm's length and still sits below the row name.
 *
 * TY_ROW_NAME started at the mockup's 11px, which left it *smaller* than the
 * subtitle it is supposed to dominate - the mockup gets that hierarchy from
 * font weight, and the 3DS system font has only one weight, so size has to
 * carry it. 14px restores the order and reads better on the Library rows. */

static float s_scale[TY_COUNT];

/* The mockup asks for 7px here. Rendered and compared side by side: at 7px the
 * label is an unreadable smudge (the glyph atlas is minified ~4x and the
 * bilinear filter finishes it off). 10px was legible, 12px is comfortable at
 * the distance a handheld is actually held, and the tracking that gives the
 * mockup its character survives either way. ui_set_micro_px keeps it
 * adjustable. */
static float s_micro_px = 12.0f;
static float s_em;              /* font em box at scale 1.0 */
static float s_ascent;
static C2D_TextBuf s_measure_buf;

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

	/* Width probes and ellipsis fitting must not consume the frame's text arena.
	 * C2D_TextParse appends on every call, even when the result is only measured. */
	s_measure_buf = C2D_TextBufNew(1024);
}

void ui_exit(void)
{
	if (s_measure_buf) {
		C2D_TextBufDelete(s_measure_buf);
		s_measure_buf = NULL;
	}
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

static float measure_width(C2D_TextBuf fallback, const char *s, type_role r)
{
	C2D_TextBuf buf = s_measure_buf ? s_measure_buf : fallback;
	if (s_measure_buf)
		C2D_TextBufClear(s_measure_buf);

	C2D_Text t;
	C2D_TextParse(&t, buf, s);
	C2D_TextOptimize(&t);
	const float sc = ui_scale(r);
	float w = 0.0f, h = 0.0f;
	C2D_TextGetDimensions(&t, sc, sc, &w, &h);
	return w;
}

static int utf8_previous(const char *s, int len)
{
	if (len <= 0)
		return 0;
	len--;
	while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80)
		len--;
	return len;
}

static void fit_text(C2D_TextBuf fallback, char *s, size_t cap, type_role r,
	                 float maxw)
{
	if (maxw <= 0.0f || measure_width(fallback, s, r) <= maxw)
		return;

	int content_len = (int)strlen(s);
	while (content_len > 0) {
		content_len = utf8_previous(s, content_len);
		while ((size_t)content_len + 3 > cap)
			content_len = utf8_previous(s, content_len);
		s[content_len] = '.';
		s[content_len + 1] = '.';
		s[content_len + 2] = '\0';
		if (measure_width(fallback, s, r) <= maxw)
			return;
	}
}

float ui_text_width(C2D_TextBuf buf, const char *s, type_role r)
{
	if (!s || !s[0])
		return 0.0f;
	return measure_width(buf, s, r);
}

void ui_text(C2D_TextBuf buf, const char *s, float x, float y, type_role r,
             float maxw, u32 clr)
{
	if (!s || !s[0])
		return;

	char tmp[256];
	snprintf(tmp, sizeof tmp, "%s", s);
	fit_text(buf, tmp, sizeof tmp, r, maxw);

	const float sc = ui_scale(r);

	C2D_Text t;
	C2D_TextParse(&t, buf, tmp);
	C2D_TextOptimize(&t);

	C2D_DrawText(&t, C2D_WithColor | C2D_AtBaseline, x, y, 0.0f, sc, sc, clr);
}

float ui_text_wrapped(C2D_TextBuf buf, const char *s, float x, float y,
                      type_role r, float maxw, u32 clr, int maxlines,
                      float leading)
{
	if (!s || !s[0] || maxlines <= 0)
		return y;

	const float step = ui_px(r) * leading;
	float       bl   = y;
	const char *p    = s;
	int         line = 0;

	while (*p && line < maxlines) {
		/* Longest run of whole words that fits. Measuring per candidate is
		 * fine here: this runs only on the error screen, not per frame of
		 * ordinary playback. */
		size_t take = 0, probe = 0;
		char   cand[256];

		for (;;) {
			while (p[probe] == ' ')
				probe++;
			while (p[probe] && p[probe] != ' ')
				probe++;
			if (probe == 0)
				break;

			snprintf(cand, sizeof cand, "%.*s", (int)probe, p);
			if (measure_width(buf, cand, r) > maxw)
				break;

			take = probe;
			if (!p[probe])
				break;
		}

		/* A single word too long for the column: hand it to ui_text, which
		 * ellipsises it, rather than looping forever on zero progress. */
		if (take == 0) {
			ui_text(buf, p, x, bl, r, maxw, clr);
			return bl;
		}

		const char *line_start = p;

		p += take;
		while (*p == ' ')
			p++;

		/* On the last permitted line with text still to come, draw the raw
		 * remainder so ui_text's ellipsis shows the reader it was cut. */
		if (*p && line == maxlines - 1) {
			ui_text(buf, line_start, x, bl, r, maxw, clr);
			return bl;
		}

		snprintf(cand, sizeof cand, "%.*s", (int)take, line_start);
		ui_text(buf, cand, x, bl, r, maxw, clr);

		line++;
		if (*p)
			bl += step;
	}

	return bl;
}

static char *find_ci(char *text, const char *needle)
{
	const size_t nn = strlen(needle);
	if (!nn)
		return NULL;
	for (char *p = text; *p; p++) {
		size_t i = 0;
		while (i < nn && p[i] &&
		       tolower((unsigned char)p[i]) ==
		           tolower((unsigned char)needle[i]))
			i++;
		if (i == nn)
			return p;
	}
	return NULL;
}

void ui_text_highlight(C2D_TextBuf buf, const char *s, const char *needle,
                       float x, float y, type_role r, float maxw, u32 clr,
                       u32 highlight_clr)
{
	if (!s || !s[0] || !needle || !needle[0]) {
		ui_text(buf, s, x, y, r, maxw, clr);
		return;
	}

	char tmp[256];
	snprintf(tmp, sizeof tmp, "%s", s);
	char *match = find_ci(tmp, needle);
	if (!match) {
		ui_text(buf, s, x, y, r, maxw, clr);
		return;
	}

	/* Keep the same ellipsis behaviour as ui_text before splitting the colour
	 * runs. A truncated-away match simply renders as ordinary text. */
	fit_text(buf, tmp, sizeof tmp, r, maxw);
	match = find_ci(tmp, needle);
	if (!match) {
		ui_text(buf, tmp, x, y, r, maxw, clr);
		return;
	}

	const size_t nn = strlen(needle);
	const size_t before_n = (size_t)(match - tmp);
	char before[256], matched[256], after[256];
	snprintf(before, sizeof before, "%.*s", (int)before_n, tmp);
	snprintf(matched, sizeof matched, "%.*s", (int)nn, match);
	snprintf(after, sizeof after, "%s", match + nn);

	const float before_w = ui_text_width(buf, before, r);
	const float match_w = ui_text_width(buf, matched, r);
	ui_text(buf, before, x, y, r, maxw, clr);
	ui_text(buf, matched, x + before_w, y, r, maxw - before_w, highlight_clr);
	ui_text(buf, after, x + before_w + match_w, y, r,
	        maxw - before_w - match_w, clr);
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
	/* Segments needed for a sagitta of a quarter pixel: r(1-cos(pi/n)) <= 0.25.
	 * Small dots are already round at low counts, so this only spends triangles
	 * where the facets would actually be visible. */
	int segs = (int)(3.5f * sqrtf(r)) * 2;
	if (segs < 10)
		segs = 10;
	else if (segs > 48)
		segs = 48;

	/* Fade the last pixel to transparent instead of ending on a hard edge. A
	 * disc smaller than that would vanish, so it stays a plain fan. */
	const float edge = r > 1.5f ? 0.75f : 0.0f;
	const float inner_r = r - edge;
	const u32 clear = clr & 0x00FFFFFF;

	float ipx = cx + inner_r, ipy = cy;
	float opx = cx + r, opy = cy;
	for (int i = 1; i <= segs; i++) {
		const float a   = (float)i * 2.0f * (float)M_PI / (float)segs;
		const float cs  = cosf(a);
		const float sn  = sinf(a);
		const float inx = cx + inner_r * cs;
		const float iny = cy + inner_r * sn;
		C2D_DrawTriangle(cx, cy, clr, ipx, ipy, clr, inx, iny, clr, 0.0f);
		if (edge > 0.0f) {
			const float onx = cx + r * cs;
			const float ony = cy + r * sn;
			C2D_DrawTriangle(ipx, ipy, clr, opx, opy, clear, onx, ony, clear,
			                 0.0f);
			C2D_DrawTriangle(ipx, ipy, clr, onx, ony, clear, inx, iny, clr,
			                 0.0f);
			opx = onx;
			opy = ony;
		}
		ipx = inx;
		ipy = iny;
	}
}

void ui_polyline(const float *pts, int count, float thickness, u32 clr)
{
	if (!pts || count < 2 || thickness <= 0.0f)
		return;

	const u32 clear = clr & 0x00FFFFFF;
	const float half = thickness / 2.0f;
	/* Keep the solid core at least a hair wide: for a thin stroke the fade
	 * would otherwise consume the whole width and leave nothing visible. */
	const float fade = half > 0.6f ? 0.5f : half * 0.4f;
	const float core = half - fade;

	/* Offset each joint along the average of its two segment normals so the
	 * two sides share vertices. Scaling by 1/cos(theta/2) keeps the stroke an
	 * even width through the bend (the standard mitre), clamped so a near
	 * reversal cannot fling the corner off to infinity. */
	float pnx = 0.0f, pny = 0.0f;
	for (int i = 0; i + 1 < count; i++) {
		const float ax = pts[i * 2], ay = pts[i * 2 + 1];
		const float bx = pts[i * 2 + 2], by = pts[i * 2 + 3];
		float dx = bx - ax, dy = by - ay;
		const float len = sqrtf(dx * dx + dy * dy);
		if (len <= 0.0001f)
			continue;
		dx /= len;
		dy /= len;
		const float nx = -dy, ny = dx;

		/* Start joint: average with the previous segment unless this is the
		 * first, which is left square. */
		float sx = nx, sy = ny;
		if (i > 0) {
			sx = nx + pnx;
			sy = ny + pny;
			const float sl = sqrtf(sx * sx + sy * sy);
			if (sl > 0.0001f) {
				sx /= sl;
				sy /= sl;
				float scale = sx * nx + sy * ny;
				if (scale < 0.35f)
					scale = 0.35f;
				sx /= scale;
				sy /= scale;
			} else {
				sx = nx;
				sy = ny;
			}
		}

		/* End joint: average with the next segment unless this is the last. */
		float ex = nx, ey = ny;
		if (i + 2 < count) {
			const float cx = pts[i * 2 + 4], cy = pts[i * 2 + 5];
			float ndx = cx - bx, ndy = cy - by;
			const float nl = sqrtf(ndx * ndx + ndy * ndy);
			if (nl > 0.0001f) {
				ndx /= nl;
				ndy /= nl;
				ex = nx + -ndy;
				ey = ny + ndx;
				const float el = sqrtf(ex * ex + ey * ey);
				if (el > 0.0001f) {
					ex /= el;
					ey /= el;
					float scale = ex * nx + ey * ny;
					if (scale < 0.35f)
						scale = 0.35f;
					ex /= scale;
					ey /= scale;
				} else {
					ex = nx;
					ey = ny;
				}
			}
		}

		const float a_in_x = ax + sx * core, a_in_y = ay + sy * core;
		const float a_out_x = ax - sx * core, a_out_y = ay - sy * core;
		const float b_in_x = bx + ex * core, b_in_y = by + ey * core;
		const float b_out_x = bx - ex * core, b_out_y = by - ey * core;

		/* Solid core. */
		C2D_DrawTriangle(a_in_x, a_in_y, clr, a_out_x, a_out_y, clr, b_in_x,
		                 b_in_y, clr, 0.0f);
		C2D_DrawTriangle(a_out_x, a_out_y, clr, b_out_x, b_out_y, clr, b_in_x,
		                 b_in_y, clr, 0.0f);

		/* Fading skirt on each side. */
		const float a_fin_x = ax + sx * half, a_fin_y = ay + sy * half;
		const float a_fout_x = ax - sx * half, a_fout_y = ay - sy * half;
		const float b_fin_x = bx + ex * half, b_fin_y = by + ey * half;
		const float b_fout_x = bx - ex * half, b_fout_y = by - ey * half;

		C2D_DrawTriangle(a_in_x, a_in_y, clr, a_fin_x, a_fin_y, clear, b_fin_x,
		                 b_fin_y, clear, 0.0f);
		C2D_DrawTriangle(a_in_x, a_in_y, clr, b_fin_x, b_fin_y, clear, b_in_x,
		                 b_in_y, clr, 0.0f);
		C2D_DrawTriangle(a_out_x, a_out_y, clr, a_fout_x, a_fout_y, clear,
		                 b_fout_x, b_fout_y, clear, 0.0f);
		C2D_DrawTriangle(a_out_x, a_out_y, clr, b_fout_x, b_fout_y, clear,
		                 b_out_x, b_out_y, clr, 0.0f);

		pnx = nx;
		pny = ny;
	}
}

void ui_now_playing_badge(float x, float y, float size, bool playing,
                          unsigned animation_ms)
{
	static const unsigned char active_heights[8][4] = {
		{7, 16, 10, 14}, {10, 18, 7, 12}, {14, 12, 9, 17}, {17, 8, 13, 11},
		{12, 10, 18, 8}, {8, 14, 15, 10}, {11, 17, 8, 14}, {9, 13, 11, 18},
	};
	static const unsigned char paused_heights[4] = {8, 16, 10, 14};
	const float scale = size / 30.0f;
	const float bar_w = 3.0f * scale;
	const float gap = 2.0f * scale;
	const float bars_w = 4.0f * bar_w + 3.0f * gap;
	const float bx = x + (size - bars_w) / 2.0f;
	const float base = y + size - 5.0f * scale;

	C2D_DrawRectSolid(x, y, 0.0f, size, size,
	                  C2D_Color32(0x00, 0x00, 0x00, 0x8C));
	for (int i = 0; i < 4; i++) {
		float height = paused_heights[i];
		if (playing) {
			const unsigned frame = (animation_ms / 180) % 8;
			const float t = (float)(animation_ms % 180) / 180.0f;
			const float t2 = t * t;
			const float t3 = t2 * t;
			const float p0 = active_heights[(frame + 7) % 8][i];
			const float p1 = active_heights[frame][i];
			const float p2 = active_heights[(frame + 1) % 8][i];
			const float p3 = active_heights[(frame + 2) % 8][i];

			/* Cyclic Catmull-Rom interpolation keeps both height and velocity
			 * continuous as each keyframe passes. */
			height = 0.5f * (2.0f * p1 + (-p0 + p2) * t +
			                 (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
			                 (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
		}
		const float h = height * scale;
		C2D_DrawRectSolid(bx + (bar_w + gap) * i, base - h, 0.0f, bar_w, h,
		                  C2D_Color32(0x1D, 0xB9, 0x54, 0xFF));
	}
}

void ui_progress_bar(long elapsed_ms, long duration_ms)
{
	const float width = 320.0f;
	const float y = 240.0f - UI_PROGRESS_BAR_H;
	float progress = 0.0f;
	if (duration_ms > 0) {
		long clamped = elapsed_ms;
		if (clamped < 0)
			clamped = 0;
		if (clamped > duration_ms)
			clamped = duration_ms;
		progress = (float)clamped / (float)duration_ms;
	}
	C2D_DrawRectSolid(0.0f, y, 0.0f, width, UI_PROGRESS_BAR_H,
	                  C2D_Color32(0x32, 0x32, 0x32, 0xFF));
	if (progress > 0.0f)
		C2D_DrawRectSolid(0.0f, y, 0.0f, width * progress,
		                  UI_PROGRESS_BAR_H,
		                  C2D_Color32(0x1D, 0xB9, 0x54, 0xFF));
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
	const int middot = fontGlyphIndexFromCodePoint(NULL, 0x00B7);
	tl_timing("font: U+00B7 glyph=%d available=%d", middot,
	          middot != (int)fi->alterCharIndex);

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
