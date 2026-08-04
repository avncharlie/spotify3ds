#include "thumbs.h"

#include <3ds.h>
#include <stdlib.h>
#include <string.h>

#include "../spotify/art.h"
#include "../testlog.h"
#include "../worker.h"

typedef struct {
	album_art art;
	u32       used; /* LRU stamp; 0 means the slot is free */
} slot;

static slot s_slots[THUMBS_SLOTS];
static u32  s_clock;

/* Requested but not yet arrived. Without this the UI would re-queue the same
 * url every frame from thumbs_get, and while worker_request_thumb does dedupe,
 * saying so here avoids the lock traffic entirely - this runs per tile per
 * frame. */
static char s_pending[THUMBS_SLOTS][256];
static int  s_pending_n;

static bool pending_has(const char *url)
{
	for (int i = 0; i < s_pending_n; i++)
		if (strcmp(s_pending[i], url) == 0)
			return true;
	return false;
}

static void pending_add(const char *url)
{
	if (s_pending_n >= THUMBS_SLOTS)
		return;
	snprintf(s_pending[s_pending_n++], sizeof s_pending[0], "%s", url);
}

static void pending_remove(const char *url)
{
	for (int i = 0; i < s_pending_n; i++) {
		if (strcmp(s_pending[i], url) != 0)
			continue;
		s_pending_n--;
		memmove(s_pending[i], s_pending[i + 1],
		        (size_t)(s_pending_n - i) * sizeof s_pending[0]);
		return;
	}
}

void thumbs_free_all(void)
{
	for (int i = 0; i < THUMBS_SLOTS; i++) {
		if (s_slots[i].used) {
			art_free(&s_slots[i].art);
			s_slots[i].used = 0;
		}
	}
	s_pending_n = 0;
}

const C2D_Image *thumbs_get(const char *url)
{
	if (!url || !url[0])
		return NULL;

	for (int i = 0; i < THUMBS_SLOTS; i++) {
		if (!s_slots[i].used || !s_slots[i].art.valid)
			continue;
		if (strcmp(s_slots[i].art.url, url) != 0)
			continue;

		s_slots[i].used = ++s_clock;
		return &s_slots[i].art.image;
	}

	if (!pending_has(url)) {
		pending_add(url);
		worker_request_thumb(url);
	}

	return NULL;
}

void thumbs_pump(void)
{
	art_payload p;
	if (!worker_take_thumb(&p))
		return;

	pending_remove(p.url);

	/* Pick a slot: a free one, else the least recently drawn. Eviction is by
	 * last *use* rather than last load, so the tiles currently on screen
	 * survive a scroll through a long list. */
	int victim = 0;
	for (int i = 0; i < THUMBS_SLOTS; i++) {
		if (!s_slots[i].used) {
			victim = i;
			break;
		}
		if (s_slots[i].used < s_slots[victim].used)
			victim = i;
	}

	if (s_slots[victim].used)
		art_free(&s_slots[victim].art);

	char       err[128];
	const bool ok =
	    p.from_cache
	        ? art_upload_tiled(&s_slots[victim].art, p.tiled, p.w, p.h,
	                           p.tex_dim, p.accent_r, p.accent_g, p.accent_b,
	                           p.url, err, sizeof err)
	        : art_upload(&s_slots[victim].art, p.rgba, p.w, p.h, p.url, err,
	                     sizeof err);

	if (ok) {
		s_slots[victim].used = ++s_clock;
		if (p.from_cache)
			p.tiled = NULL; /* art_upload_tiled took ownership */
	} else {
		s_slots[victim].used = 0;
		tl_log("thumb upload failed: %s", err);
	}

	art_payload_free(&p);
}
