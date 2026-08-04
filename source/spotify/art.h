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

/* Texture dimension: power of two, >= the decoded image.
 *
 * WARNING: changing this invalidates every entry in the SD art cache, because
 * the stored payload is tile-rows sized against it. Bump ARTCACHE_VERSION in
 * artcache.h at the same time. (artcache_load also checks tex_dim per entry,
 * so a forgotten bump degrades to a silent full refetch rather than corruption
 * - but bump it anyway.) */
#define ART_TEX_SIZE 256

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

/* Publish an already-tiled buffer (from the SD cache), skipping both the
 * Morton tiling and the accent extraction - the accent is supplied because it
 * cannot be recovered from tiled data. Takes ownership of `tiled` and
 * linearFrees it. Render thread only. */
bool art_upload_tiled(album_art *a, u8 *tiled, int w, int h, u8 accent_r,
                      u8 accent_g, u8 accent_b, const char *url, char *err,
                      int errlen);

/* Shared with artcache.c, which needs to produce the same tiled layout.
 * Writes a full dim*dim*4 buffer; see art.c for the Morton layout. */
void art_tile_rgba(const u8 *src, int src_w, int src_h, u8 *dst, int dim);

/* Dominant colour of linear RGBA, written to a->accent_*. Exposed because the
 * cache must compute it before tiling: it cannot be recovered from tiled data,
 * which is why entries store it in their header. */
void art_accent_of(const u8 *rgba, int w, int h, album_art *a);

void art_free(album_art *a);
