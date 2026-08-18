#include "touch.h"

#include <3ds.h>
#include <stdlib.h>

void tb_reset(touch_builder *tb)
{
	tb->n = 0;
}

void tb_add(touch_builder *tb, float x, float y, float w, float h, int id)
{
	if (tb->n >= TOUCH_MAX_RECTS)
		return; /* silently drop rather than corrupt: a missing hit area is a
		         * far milder failure than an overrun */
	tb->rects[tb->n++] = (touch_rect){x, y, w, h, id, false};
}

void tb_add_hold(touch_builder *tb, float x, float y, float w, float h, int id)
{
	if (tb->n >= TOUCH_MAX_RECTS)
		return;
	tb->rects[tb->n++] = (touch_rect){x, y, w, h, id, true};
}

void tb_add_hit(touch_builder *tb, float cx, float cy, float min_size, int id)
{
	const float s = min_size < 44.0f ? 44.0f : min_size;
	tb_add(tb, cx - s / 2.0f, cy - s / 2.0f, s, s, id);
}

static const touch_rect *rect_at(const touch_rect *rects, int nrects, int x,
	                             int y)
{
	for (int i = 0; i < nrects; i++) {
		const touch_rect *r = &rects[i];
		if ((float)x >= r->x && (float)x < r->x + r->w && (float)y >= r->y &&
		    (float)y < r->y + r->h)
			return r;
	}
	return NULL;
}

int touch_hit(const touch_rect *rects, int nrects, int x, int y)
{
	for (int i = 0; i < nrects; i++) {
		const touch_rect *r = &rects[i];
		if ((float)x >= r->x && (float)x < r->x + r->w && (float)y >= r->y &&
		    (float)y < r->y + r->h)
			return r->id;
	}
	return -1;
}

void touch_update(touch_state *t, const touch_rect *rects, int nrects)
{
	const u32 down = hidKeysHeld() & KEY_TOUCH;
	const u32 hit  = hidKeysDown() & KEY_TOUCH;
	const u32 up   = hidKeysUp() & KEY_TOUCH;

	t->pressed  = false;
	t->released = false;
	t->clicked  = -1;
	t->long_pressed = -1;
	t->dx       = 0;
	t->dy       = 0;

	if (down) {
		/* Only read coordinates while held: on the release frame the hardware
		 * has already zeroed them, so the latched value is what we use. */
		touchPosition tp;
		hidTouchRead(&tp);
		if (tp.px || tp.py) {
			if (!hit) {
				t->dx = (int)tp.px - t->px;
				t->dy = (int)tp.py - t->py;
			}
			t->px = tp.px;
			t->py = tp.py;
		}
	}

	if (hit) {
		t->down     = true;
		t->pressed  = true;
		t->dragging = false;
		t->press_at = osGetTime();
		t->tap_cancelled = false;
		t->long_fired = false;
		t->start_px = t->px;
		t->start_py = t->py;
		const touch_rect *pr = rect_at(rects, nrects, t->px, t->py);
		t->press_id = pr ? pr->id : -1;
		t->press_hold = pr && pr->hold;
	}

	if (down && !t->dragging) {
		const int mx = abs(t->px - t->start_px);
		const int my = abs(t->py - t->start_py);
		if (mx > TOUCH_SLOP || my > TOUCH_SLOP)
			t->dragging = true;
	}

	/* A held finger is usually waiting to scroll, not asking to activate a row.
	 * Keep drag detection alive, but disarm the eventual release as a tap.
	 *
	 * Not for a rect that answers a hold, though: there is no scroll gesture
	 * on a button for a slow release to be mistaken for, and disarming it
	 * would leave every release between this timeout and the long press doing
	 * nothing whatsoever. */
	if (down && !t->press_hold &&
	    osGetTime() - t->press_at > TOUCH_TAP_TIMEOUT_MS)
		t->tap_cancelled = true;

	if (down && !t->dragging && !t->long_fired && t->press_id >= 0 &&
	    osGetTime() - t->press_at >= TOUCH_LONG_PRESS_MS &&
	    touch_hit(rects, nrects, t->px, t->py) == t->press_id) {
		t->long_pressed = t->press_id;
		t->long_fired = true;
		t->tap_cancelled = true;
	}

	if (up) {
		t->down     = false;
		t->released = true;

		/* Fire only if the release lands in the same rect as the press, so
		 * sliding off cancels - and never after a drag, or scrolling a list
		 * would play whatever row the finger lifted over. */
		if (!t->dragging && !t->tap_cancelled) {
			const int rel = touch_hit(rects, nrects, t->px, t->py);
			if (rel >= 0 && rel == t->press_id)
				t->clicked = rel;
		}

		t->press_id = -1;
		t->press_hold = false;
	}
}
