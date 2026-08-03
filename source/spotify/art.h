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
} album_art;

/* Fetch and decode the art at `url` into `a`, replacing whatever it held.
 * A no-op returning true when `url` already matches what is loaded, so the
 * caller can call this every poll without refetching. */
bool art_load(album_art *a, const char *url, char *err, int errlen);

void art_free(album_art *a);
