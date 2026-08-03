#include "art.h"

#include <3ds.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../net/http.h"
#include "../testlog.h"

#define ART_TEX_SIZE 256 /* power of two, >= decoded size */

/* --- libjpeg error handling ---------------------------------------------
 * The default handler calls exit(), which would kill the app on a truncated
 * or corrupt image. Longjmp back instead so a bad fetch is just a failed load.
 */
struct jerr_mgr {
	struct jpeg_error_mgr pub;
	jmp_buf               escape;
};

static void jerr_exit(j_common_ptr cinfo)
{
	struct jerr_mgr *m = (struct jerr_mgr *)cinfo->err;
	longjmp(m->escape, 1);
}

static void jerr_silent(j_common_ptr cinfo)
{
	(void)cinfo; /* swallow warnings; Spotify JPEGs are well-formed */
}

/* --- GPU tiling ----------------------------------------------------------
 * The PICA200 stores textures as 8x8 tiles, and within each tile the pixels
 * follow a Morton (Z-order) curve. C3D_TexLoadImage expects data already in
 * that layout, so a linear RGBA buffer has to be shuffled first.
 *
 * Byte order in memory is ABGR (i.e. reversed RGBA).
 */
static inline u32 morton_offset(u32 x, u32 y)
{
	/* Interleave the low 3 bits of x and y into a Z-order index. */
	u32 i = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
	        ((x & 4) << 2) | ((y & 4) << 3);
	return i;
}

static void tile_rgba(const u8 *src, int src_w, int src_h, u8 *dst, int dim)
{
	/* Transparent background so non-square art letterboxes cleanly. */
	memset(dst, 0, (size_t)dim * dim * 4);

	for (int y = 0; y < src_h; y++) {
		for (int x = 0; x < src_w; x++) {
			const u32 tile_x = x / 8, tile_y = y / 8;
			const u32 tiles_per_row = dim / 8;
			const u32 tile_index = tile_y * tiles_per_row + tile_x;
			const u32 within     = morton_offset(x & 7, y & 7);
			const u32 dst_px     = tile_index * 64 + within;

			const u8 *s = src + ((size_t)y * src_w + x) * 4;
			u8       *d = dst + (size_t)dst_px * 4;

			/* RGBA -> ABGR */
			d[0] = s[3];
			d[1] = s[2];
			d[2] = s[1];
			d[3] = s[0];
		}
	}
}

/* Split "https://i.scdn.co/image/<id>" into host and path. */
static bool split_url(const char *url, char *host, int hostlen, char *path,
                      int pathlen)
{
	const char *p = strstr(url, "://");
	if (!p)
		return false;
	p += 3;

	const char *slash = strchr(p, '/');
	if (!slash)
		return false;

	int hl = (int)(slash - p);
	if (hl >= hostlen)
		return false;
	memcpy(host, p, hl);
	host[hl] = '\0';

	snprintf(path, pathlen, "%s", slash);
	return true;
}

void art_free(album_art *a)
{
	if (!a || !a->valid)
		return;
	C3D_TexDelete(&a->tex);
	a->valid  = false;
	a->url[0] = '\0';
}

bool art_load(album_art *a, const char *url, char *err, int errlen)
{
	if (!url || !url[0]) {
		snprintf(err, errlen, "empty url");
		return false;
	}

	/* Art only changes on track change; skip the fetch otherwise. */
	if (a->valid && strcmp(a->url, url) == 0)
		return true;

	char host[128], path[256];
	if (!split_url(url, host, sizeof host, path, sizeof path)) {
		snprintf(err, errlen, "bad url");
		return false;
	}

	http_response r;
	if (!http_request(host, "GET", path, NULL, NULL, NULL, &r, err, errlen))
		return false;

	if (r.status != 200 || !r.body || r.body_len == 0) {
		snprintf(err, errlen, "art http %d", r.status);
		http_free(&r);
		return false;
	}

	const u64 t0 = osGetTime();

	/* --- decode ------------------------------------------------------- */
	struct jpeg_decompress_struct cinfo;
	struct jerr_mgr               jerr;
	u8                           *linear = NULL;

	cinfo.err           = jpeg_std_error(&jerr.pub);
	jerr.pub.error_exit = jerr_exit;
	jerr.pub.output_message = jerr_silent;

	if (setjmp(jerr.escape)) {
		jpeg_destroy_decompress(&cinfo);
		free(linear);
		http_free(&r);
		snprintf(err, errlen, "jpeg decode failed");
		return false;
	}

	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, (const unsigned char *)r.body, r.body_len);
	jpeg_read_header(&cinfo, TRUE);

	/* Scale during decode: 640x640 -> 160x160 in the DCT domain, far cheaper
	 * than decoding full size and resampling afterwards. */
	cinfo.scale_num       = 1;
	cinfo.scale_denom     = 4;
	cinfo.out_color_space = JCS_EXT_RGBA;

	jpeg_start_decompress(&cinfo);

	const int w = (int)cinfo.output_width;
	const int h = (int)cinfo.output_height;

	if (w > ART_TEX_SIZE || h > ART_TEX_SIZE) {
		jpeg_abort_decompress(&cinfo);
		jpeg_destroy_decompress(&cinfo);
		http_free(&r);
		snprintf(err, errlen, "decoded %dx%d exceeds %d", w, h, ART_TEX_SIZE);
		return false;
	}

	linear = malloc((size_t)w * h * 4);
	if (!linear) {
		jpeg_abort_decompress(&cinfo);
		jpeg_destroy_decompress(&cinfo);
		http_free(&r);
		snprintf(err, errlen, "oom for %dx%d", w, h);
		return false;
	}

	while (cinfo.output_scanline < cinfo.output_height) {
		u8 *row = linear + (size_t)cinfo.output_scanline * w * 4;
		jpeg_read_scanlines(&cinfo, &row, 1);
	}

	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	http_free(&r);

	/* --- upload -------------------------------------------------------- */
	u8 *tiled = linearAlloc((size_t)ART_TEX_SIZE * ART_TEX_SIZE * 4);
	if (!tiled) {
		free(linear);
		snprintf(err, errlen, "linearAlloc failed");
		return false;
	}

	tile_rgba(linear, w, h, tiled, ART_TEX_SIZE);
	free(linear);

	art_free(a); /* release any previous texture before replacing it */

	if (!C3D_TexInit(&a->tex, ART_TEX_SIZE, ART_TEX_SIZE, GPU_RGBA8)) {
		linearFree(tiled);
		snprintf(err, errlen, "C3D_TexInit failed");
		return false;
	}

	C3D_TexLoadImage(&a->tex, tiled, GPU_TEXFACE_2D, 0);
	C3D_TexSetFilter(&a->tex, GPU_LINEAR, GPU_LINEAR);
	linearFree(tiled);

	/* Only the top-left w*h of the texture holds image data. */
	a->sub.width  = (u16)w;
	a->sub.height = (u16)h;
	a->sub.left   = 0.0f;
	a->sub.top    = 1.0f;
	a->sub.right  = (float)w / ART_TEX_SIZE;
	a->sub.bottom = 1.0f - (float)h / ART_TEX_SIZE;

	a->image.tex    = &a->tex;
	a->image.subtex = &a->sub;
	a->src_w        = w;
	a->src_h        = h;
	a->decode_ms    = (unsigned)(osGetTime() - t0);
	a->valid        = true;
	snprintf(a->url, sizeof a->url, "%s", url);

	return true;
}
