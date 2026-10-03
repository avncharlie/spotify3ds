#include "thumbs.h"

#include <3ds.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "../spotify/art.h"
#include "../testlog.h"
#include "../worker.h"

typedef struct {
	album_art art;
	u32       used; /* LRU stamp; 0 means the slot is free */
	time_t    expires_at;
} slot;

static slot s_slots[THUMBS_SLOTS];
static u32  s_clock;

/* Requested but not yet arrived. Without this the UI would re-queue the same
 * url every frame from thumbs_get, and while worker_request_thumb does dedupe,
 * saying so here avoids the lock traffic entirely - this runs per tile per
 * frame. */
static char s_pending[THUMBS_SLOTS][256];
static int  s_pending_n;

typedef struct {
	char url[256];
	unsigned attempts;
	u64 retry_at;
} failed_thumb;
static failed_thumb s_failed[THUMBS_SLOTS];
static unsigned s_failed_next;

static failed_thumb *failure_for(const char *url)
{
	for (int i = 0; i < THUMBS_SLOTS; i++)
		if (strcmp(s_failed[i].url, url) == 0)
			return &s_failed[i];
	return NULL;
}

static void record_failure(const char *url)
{
	failed_thumb *failed = failure_for(url);
	if (!failed) {
		failed = &s_failed[s_failed_next++ % THUMBS_SLOTS];
		memset(failed, 0, sizeof *failed);
		snprintf(failed->url, sizeof failed->url, "%s", url);
	}
	failed->attempts++;
	failed->retry_at = osGetTime() + (u64)failed->attempts * 30000;
}

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
	memset(s_failed, 0, sizeof s_failed);
	s_failed_next = 0;
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

	failed_thumb *failed = failure_for(url);
	if (failed && (failed->attempts >= 3 || osGetTime() < failed->retry_at))
		return NULL;
	if (!pending_has(url) && s_pending_n < THUMBS_SLOTS) {
		if (worker_request_thumb(url))
			pending_add(url);
	}

	return NULL;
}

void thumbs_pump(void)
{
	/* Called after the frame's GPU synchronization, before any image draws.
	 * Getters during rendering are non-destructive, including duplicate rows
	 * whose expiry deadline falls between their two lookups. */
	time_t now = time(NULL);
	for (int i = 0; i < THUMBS_SLOTS; i++)
		if (s_slots[i].used && s_slots[i].expires_at && now >= s_slots[i].expires_at) {
			art_free(&s_slots[i].art);
			s_slots[i].used = 0;
		}
	art_payload p;
	if (!worker_take_thumb(&p))
		return;

	pending_remove(p.url);
	if (p.failed) {
		record_failure(p.url);
		art_payload_free(&p);
		return;
	}

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
	if (p.from_cache)
		p.tiled = NULL; /* upload consumes the buffer on both success and failure */

	if (ok) {
		s_slots[victim].used = ++s_clock;
		s_slots[victim].expires_at = p.expires_at;
		failed_thumb *failed = failure_for(p.url);
		if (failed)
			memset(failed, 0, sizeof *failed);
	} else {
		s_slots[victim].used = 0;
		record_failure(p.url);
		tl_log("thumb upload failed: %s", err);
	}

	art_payload_free(&p);
}
