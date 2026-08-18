#include "volume_overlay.h"

#include <stdio.h>

#include "ui.h"

#define BOT_W 320.0f
#define BOT_H 240.0f
#define PANEL_X 60.0f
#define PANEL_Y 83.0f
#define PANEL_W 200.0f
#define PANEL_H 74.0f

static u32 color(u8 r, u8 g, u8 b, u8 alpha, u8 fade)
{
	return C2D_Color32(r, g, b, (u8)((unsigned)alpha * fade / 255));
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

/* The arc is a quadratic Bezier. Eight chords already track it to within
 * 0.05px, so the choppiness was never the flattening - it was drawing them as
 * separate hard-edged quads. Stroking the whole curve as one anti-aliased
 * strip removes both the stair-stepping and the notches at the joints. */
#define WAVE_SEGMENTS 8

static void draw_wave(float x, float cy, float radius, float bulge, u32 clr)
{
	float pts[(WAVE_SEGMENTS + 1) * 2];
	for (int i = 0; i <= WAVE_SEGMENTS; i++) {
		const float t = (float)i / (float)WAVE_SEGMENTS;
		const float inv = 1.0f - t;
		pts[i * 2] = inv * inv * x + 2.0f * inv * t * (x + bulge) + t * t * x;
		pts[i * 2 + 1] = inv * inv * (cy - radius) + 2.0f * inv * t * cy +
		                 t * t * (cy + radius);
	}
	ui_polyline(pts, WAVE_SEGMENTS + 1, 2.0f, clr);
}

static void draw_speaker(float x, float cy, bool waves, u32 clr)
{
	C2D_DrawRectSolid(x, cy - 4.0f, 0.0f, 5.0f, 8.0f, clr);
	C2D_DrawTriangle(x + 5.0f, cy - 4.0f, clr, x + 11.0f, cy - 9.0f, clr,
	                 x + 11.0f, cy + 9.0f, clr, 0.0f);
	C2D_DrawTriangle(x + 5.0f, cy - 4.0f, clr, x + 11.0f, cy + 9.0f, clr,
	                 x + 5.0f, cy + 4.0f, clr, 0.0f);

	if (waves) {
		draw_wave(x + 14.0f, cy, 5.5f, 4.0f, clr);
		draw_wave(x + 17.0f, cy, 9.0f, 6.0f, clr);
	} else {
		const float down[4] = {x + 15.5f, cy - 5.0f, x + 22.0f, cy + 5.0f};
		const float up[4] = {x + 22.0f, cy - 5.0f, x + 15.5f, cy + 5.0f};
		ui_polyline(down, 2, 2.2f, clr);
		ui_polyline(up, 2, 2.2f, clr);
	}
}

void volume_overlay_draw(const volume_overlay_args *a)
{
	if (!a || !a->alpha)
		return;
	const u8 fade = a->alpha;
	const u32 dim = color(0, 0, 0, 0x98, fade);
	const u32 shadow = color(0, 0, 0, 0xA8, fade);
	const u32 border = color(0x38, 0x38, 0x38, 0xFF, fade);
	const u32 panel = color(0x1B, 0x1B, 0x1B, 0xFF, fade);
	const u32 white = color(0xF4, 0xF4, 0xF4, 0xFF, fade);
	const u32 green = color(0x1D, 0xB9, 0x54, 0xFF, fade);
	const u32 muted = color(0x8A, 0x8A, 0x8A, 0xFF, fade);
	const u32 empty = color(0x35, 0x35, 0x35, 0xFF, fade);
	/* Center each complete content group, including the disabled caption. */
	const float content_dy = a->supported ? 4.0f : -4.0f;

	C2D_DrawRectSolid(0, 0, 0.0f, BOT_W, BOT_H, dim);
	rounded_rect(PANEL_X + 3.0f, PANEL_Y + 5.0f, PANEL_W, PANEL_H, 10.0f,
	             shadow);
	rounded_rect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 10.0f, border);
	rounded_rect(PANEL_X + 1.0f, PANEL_Y + 1.0f, PANEL_W - 2.0f,
	             PANEL_H - 2.0f, 9.0f, panel);

	draw_speaker(PANEL_X + 21.0f, PANEL_Y + 23.0f + content_dy,
	             a->supported && a->volume_percent > 0,
	             a->supported ? white : muted);
	ui_text(a->buf, "VOLUME", PANEL_X + 54.0f,
	        ui_baseline(PANEL_Y + 16.0f + content_dy, TY_ROW_NAME), TY_ROW_NAME,
	        82.0f,
	        a->supported ? muted : color(0x70, 0x70, 0x70, 0xFF, fade));

	if (a->supported) {
		char value[8];
		snprintf(value, sizeof value, "%d%%", a->volume_percent);
		const float value_w = ui_text_width(a->buf, value, TY_TITLE);
		ui_text(a->buf, value, PANEL_X + PANEL_W - 18.0f - value_w,
		        ui_baseline(PANEL_Y + 12.0f + content_dy, TY_TITLE), TY_TITLE,
		        value_w, white);
	}

	int filled = a->supported ? (a->volume_percent + 5) / 10 : 0;
	if (filled < 0)
		filled = 0;
	if (filled > 10)
		filled = 10;
	const float seg_x = PANEL_X + 21.0f;
	const float seg_y = PANEL_Y + 45.0f + content_dy;
	const float seg_w = 12.0f;
	const float seg_h = a->supported ? 8.0f : 6.0f;
	const float seg_gap = 4.0f;
	for (int i = 0; i < 10; i++)
		C2D_DrawRectSolid(seg_x + i * (seg_w + seg_gap), seg_y, 0.0f,
		                  seg_w, seg_h, i < filled ? green : empty);

	if (!a->supported) {
		char caption[96];
		if (a->device_name && a->device_name[0])
			snprintf(caption, sizeof caption, "Set volume on %s", a->device_name);
		else
			snprintf(caption, sizeof caption, "No active Spotify device");
		ui_disc(PANEL_X + 23.0f, PANEL_Y + 64.0f + content_dy, 2.2f, muted);
		ui_text(a->buf, caption, PANEL_X + 31.0f,
		        ui_baseline(PANEL_Y + 58.0f + content_dy, TY_MICRO), TY_MICRO,
		        PANEL_W - 48.0f, muted);
	}
}
