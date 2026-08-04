#pragma once

#include <stdbool.h>

/* Touch handling for the bottom screen.
 *
 * The 3DS reports a single touch point with no gestures. Two hardware quirks
 * shape this API:
 *
 *  - On release, hidTouchRead() returns (0,0): the coordinates are already
 *    gone. So the last valid position is latched while the touch is held.
 *  - The screen is resistive and pressed with a thumb, so hit rects are padded
 *    well beyond the drawn artwork.
 *
 * Buttons fire on release inside the same rect the press started in, which is
 * the standard affordance letting a user slide off to cancel.
 */

typedef struct {
	float x, y, w, h;
	int   id;
} touch_rect;

/* Hit rects are rebuilt every frame rather than held in a static table,
 * because the bottom screen now has two views and one of them scrolls. Each
 * screen registers its rects as it draws, so a glyph and its hit area are
 * written in one place and cannot drift apart. */
#define TOUCH_MAX_RECTS 32

typedef struct {
	touch_rect rects[TOUCH_MAX_RECTS];
	int        n;
} touch_builder;

void tb_reset(touch_builder *tb);
void tb_add(touch_builder *tb, float x, float y, float w, float h, int id);

/* Centre-anchored rect, padded out to at least 44x44 whatever the glyph size.
 *
 * These deliberately overlap on a tightly spaced control row, and touch_hit
 * returns the FIRST match - so registration order decides who wins a contested
 * pixel. Register centre-outward (play, then prev/next, then shuffle/repeat)
 * so the largest, most-tapped control takes precedence. */
void tb_add_hit(touch_builder *tb, float cx, float cy, float min_size, int id);

/* A drag must not also fire a tap, or scrolling a list plays whatever row the
 * finger happened to lift over. A resistive panel under a thumb jitters
 * several pixels at constant pressure, so the threshold is larger than the
 * 4-5px usual on capacitive screens. Tune on hardware: Azahar's mouse has none
 * of that jitter. */
#define TOUCH_SLOP 8
#define TOUCH_TAP_TIMEOUT_MS 300

typedef struct {
	bool down;     /* touch currently held */
	bool pressed;  /* went down this frame */
	bool released; /* came up this frame */
	int  px, py;   /* latched position (valid on release too) */
	int  start_px, start_py; /* where the press began */
	int  dx, dy;   /* movement since last frame, for dragging */
	bool dragging; /* exceeded TOUCH_SLOP since the press */
	unsigned long long press_at; /* osGetTime() when the touch began */
	bool tap_cancelled;          /* held too long to still count as a tap */
	int  press_id; /* rect the press started in, or -1 */
	int  clicked;  /* rect activated this frame, or -1 */
} touch_state;

/* Call once per frame, after hidScanInput(). */
void touch_update(touch_state *t, const touch_rect *rects, int nrects);

/* Which rect contains (x,y), or -1. First match wins; see tb_add_hit. */
int touch_hit(const touch_rect *rects, int nrects, int x, int y);
