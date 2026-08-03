#pragma once

#include <citro2d.h>
#include <stdbool.h>

/* Album art fetch + decode + GPU upload.
 *
 * Spotify serves 640x640 baseline JPEG (~41KB). We decode at 1/4 scale
 * straight out of the DCT (160x160), which costs a fraction of a full decode
 * plus resample, then tile it into a 256x256 RGBA texture because the GPU
 * requires power-of-two dimensions and 8x8 Morton-order swizzling.
 */

typedef struct {
	C3D_Tex            tex;
	Tex3DS_SubTexture  sub;
	C2D_Image          image;
	bool               valid;
	char               url[256]; /* what is currently loaded */
	int                src_w, src_h;
	unsigned           decode_ms;

	/* Dominant colour of the cover, darkened for use as a background wash.
	 * Extracted from the already-decoded pixels, so it costs nothing extra. */
	u8 accent_r, accent_g, accent_b;
} album_art;

/* Fetch and decode the art at `url` into `a`, replacing whatever it held.
 * A no-op returning true when `url` already matches what is loaded, so the
 * caller can call this every poll without refetching.
 *
 * Blocking: does network I/O. Only safe off the render thread. */
bool art_load(album_art *a, const char *url, char *err, int errlen);

/* --- split path, for loading via the worker thread --------------------
 * art_fetch_decode does the slow part (HTTP + JPEG) and returns a plain RGBA
 * buffer, so it can run anywhere. art_upload does the GPU work, which must
 * happen on the thread that owns the graphics context. */

/* Blocking fetch + decode. On success *out_rgba is malloc'd (caller frees). */
bool art_fetch_decode(const char *url, unsigned char **out_rgba, int *out_w,
                      int *out_h, unsigned *fetch_ms, unsigned *decode_ms,
                      char *err, int errlen);

/* Tile into a texture and publish as `a`. Cheap; render thread only. */
bool art_upload(album_art *a, const unsigned char *rgba, int w, int h,
                const char *url, char *err, int errlen);

void art_free(album_art *a);
