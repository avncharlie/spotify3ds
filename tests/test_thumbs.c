#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ui/thumbs.h"
#include "spotify/art.h"
#include "worker.h"

static unsigned requests, uploads, frees;
static bool queue_accept = true, upload_ok = true, ready;
static u64 milliseconds = 1000;
static time_t seconds = 100;
static art_payload payload;

u64 osGetTime(void) { return milliseconds; }
time_t time(time_t *out)
{
	if (out)
		*out = seconds;
	return seconds;
}
void tl_log(const char *format, ...) { (void)format; }

bool worker_request_thumb(const char *url)
{
	assert(url[0]);
	if (queue_accept)
		requests++;
	return queue_accept;
}
bool worker_take_thumb(art_payload *out)
{
	if (!ready)
		return false;
	*out = payload;
	memset(&payload, 0, sizeof payload);
	ready = false;
	return true;
}
void art_payload_free(art_payload *p)
{
	free(p->rgba);
	free(p->tiled);
	memset(p, 0, sizeof *p);
}
void art_free(album_art *art)
{
	if (art->valid)
		frees++;
	memset(art, 0, sizeof *art);
}
bool art_upload(album_art *art, const unsigned char *rgba, int w, int h,
                 const char *url, char *err, int errlen)
{
	(void)err;
	(void)errlen;
	assert(rgba && w > 0 && h > 0);
	uploads++;
	if (!upload_ok)
		return false;
	art->valid = true;
	snprintf(art->url, sizeof art->url, "%s", url);
	art->sub.width = (u16)w;
	art->sub.height = (u16)h;
	art->image.subtex = &art->sub;
	return true;
}
bool art_upload_tiled(album_art *art, u8 *tiled, int w, int h, int dim,
                       u8 ar, u8 ag, u8 ab, const char *url, char *err, int errlen)
{
	(void)dim;
	(void)ar;
	(void)ag;
	(void)ab;
	bool ok = art_upload(art, tiled, w, h, url, err, errlen);
	free(tiled); /* Production consumes this buffer even on texture-init failure. */
	return ok;
}
static void complete(const char *url, bool failed, bool cache, time_t expiry)
{
	memset(&payload, 0, sizeof payload);
	snprintf(payload.url, sizeof payload.url, "%s", url);
	payload.failed = failed;
	payload.from_cache = cache;
	payload.expires_at = expiry;
	payload.w = payload.h = payload.tex_dim = 64;
	if (!failed) {
		unsigned char *pixels = malloc(64 * 64 * 4);
		assert(pixels);
		if (cache)
			payload.tiled = pixels;
		else
			payload.rgba = pixels;
	}
	ready = true;
	thumbs_pump();
}
static void reset(void)
{
	thumbs_free_all();
	requests = uploads = frees = 0;
	milliseconds = 1000;
	seconds = 100;
	queue_accept = upload_ok = true;
}

int main(void)
{
	const char *url = "https://pickasso.spotifycdn.com/test/en";
	reset();
	queue_accept = false;
	assert(!thumbs_get(url));
	queue_accept = true;
	assert(!thumbs_get(url) && requests == 1); /* no stuck pending on a full queue */
	assert(!thumbs_get(url) && requests == 1);
	complete(url, true, false, 0);
	assert(!thumbs_get(url) && requests == 1);
	milliseconds += 30000;
	assert(!thumbs_get(url) && requests == 2);
	complete(url, true, false, 0);
	milliseconds += 60000;
	assert(!thumbs_get(url) && requests == 3);
	complete(url, true, false, 0);
	milliseconds += 1000000;
	assert(!thumbs_get(url) && requests == 3); /* bounded failed downloads */

	reset();
	upload_ok = false;
	assert(!thumbs_get(url) && requests == 1);
	complete(url, false, true, 0); /* ASan catches ownership/double-free mistakes */
	assert(uploads == 1 && !thumbs_get(url) && requests == 1);
	milliseconds += 30000;
	assert(!thumbs_get(url) && requests == 2);
	complete(url, false, true, 0);
	milliseconds += 60000;
	assert(!thumbs_get(url) && requests == 3);
	complete(url, false, false, 0);
	milliseconds += 1000000;
	assert(!thumbs_get(url) && requests == 3); /* bounded GPU upload failures too */

	reset();
	assert(!thumbs_get(url));
	complete(url, false, true, 110); /* original SD expiry, not a new TTL on load */
	assert(thumbs_get(url));
	seconds = 109;
	assert(thumbs_get(url) && requests == 1);
	seconds = 110;
	assert(thumbs_get(url) && thumbs_get(url) && frees == 0); /* two draws in one frame */
	thumbs_pump(); /* synchronized maintenance point, before the next frame draws */
	assert(!thumbs_get(url) && requests == 2 && frees == 1);
	complete(url, false, false, 120);
	assert(thumbs_get(url));
	thumbs_free_all();
	puts("thumbnails: queue saturation, download/upload retries, ownership and RAM expiry passed");
	return 0;
}
