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

typedef struct {
	bool  down;      /* touch currently held */
	bool  pressed;   /* went down this frame */
	bool  released;  /* came up this frame */
	int   px, py;    /* latched position (valid on release too) */
	int   press_id;  /* rect the press started in, or -1 */
	int   clicked;   /* rect activated this frame, or -1 */
} touch_state;

/* Call once per frame, after hidScanInput(). */
void touch_update(touch_state *t, const touch_rect *rects, int nrects);

/* Which rect contains (x,y), or -1. */
int touch_hit(const touch_rect *rects, int nrects, int x, int y);
