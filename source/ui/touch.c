#include "touch.h"

#include <3ds.h>

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

	if (down) {
		/* Only read coordinates while held: on the release frame the hardware
		 * has already zeroed them, so the latched value is what we use. */
		touchPosition tp;
		hidTouchRead(&tp);
		if (tp.px || tp.py) {
			t->px = tp.px;
			t->py = tp.py;
		}
	}

	if (hit) {
		t->down     = true;
		t->pressed  = true;
		t->press_id = touch_hit(rects, nrects, t->px, t->py);
	}

	if (up) {
		t->down     = false;
		t->released = true;

		/* Fire only if the release lands in the same rect as the press, so
		 * sliding off cancels. */
		const int rel = touch_hit(rects, nrects, t->px, t->py);
		if (rel >= 0 && rel == t->press_id)
			t->clicked = rel;

		t->press_id = -1;
	}
}
