#include "art.h"

#include <3ds.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../net/http.h"
#include "../testlog.h"


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

void art_tile_rgba(const u8 *src, int src_w, int src_h, u8 *dst, int dim)
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

/* --- accent colour -------------------------------------------------------
 * A flat mean over all pixels trends to muddy grey, because opposing hues
 * cancel. Weighting each pixel by its saturation instead lets the colours a
 * person would actually name dominate, while near-grey pixels contribute
 * little. Very dark and blown-out pixels are skipped so black borders and
 * white backgrounds do not wash the result out.
 */
void art_accent_of(const u8 *rgba, int w, int h, album_art *a)
{
	u64 acc_r = 0, acc_g = 0, acc_b = 0, acc_w = 0;

	/* Every 4th pixel in both axes: 1/16th the work, same answer. */
	for (int y = 0; y < h; y += 4) {
		for (int x = 0; x < w; x += 4) {
			const u8 *p = rgba + ((size_t)y * w + x) * 4;
			const int r = p[0], g = p[1], b = p[2];

			const int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
			const int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
			if (mx < 32 || mn > 232)
				continue; /* near-black or near-white */

			const u32 weight = (u32)(mx - mn) + 1; /* saturation */
			acc_r += (u64)r * weight;
			acc_g += (u64)g * weight;
			acc_b += (u64)b * weight;
			acc_w += weight;
		}
	}

	if (!acc_w) {
		/* Monochrome or empty cover: fall back to a neutral slate. */
		a->accent_r = a->accent_g = a->accent_b = 40;
		return;
	}

	int r = (int)(acc_r / acc_w);
	int g = (int)(acc_g / acc_w);
	int b = (int)(acc_b / acc_w);

	/* Darken so white text stays readable over it: the wash should read as
	 * atmosphere, not as a block of colour competing with the text. */
	r = r * 45 / 100;
	g = g * 45 / 100;
	b = b * 45 / 100;

	a->accent_r = (u8)r;
	a->accent_g = (u8)g;
	a->accent_b = (u8)b;
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

bool art_fetch_decode(const char *url, int target, unsigned char **out_rgba,
                      int *out_w, int *out_h, unsigned *fetch_ms,
                      unsigned *decode_ms, char *err, int errlen)
{
	*out_rgba = NULL;

	if (target <= 0)
		target = ART_HERO_PX;

	if (!url || !url[0]) {
		snprintf(err, errlen, "empty url");
		return false;
	}

	char host[128], path[256];
	if (!split_url(url, host, sizeof host, path, sizeof path)) {
		snprintf(err, errlen, "bad url");
		return false;
	}

	const u64 t_fetch = osGetTime();

	http_response r;
	if (!http_request(host, "GET", path, NULL, NULL, NULL, &r, err, errlen))
		return false;

	if (r.status != 200 || !r.body || r.body_len == 0) {
		snprintf(err, errlen, "art http %d", r.status);
		http_free(&r);
		return false;
	}

	if (fetch_ms)
		*fetch_ms = (unsigned)(osGetTime() - t_fetch);

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

	/* Scale during decode, in the DCT domain - far cheaper than decoding full
	 * size and resampling afterwards.
	 *
	 * The denominator is derived from the actual source size rather than fixed,
	 * because the shelf asks for Spotify's 64px variant: a hardcoded /4 turned
	 * those into unusable 16x16. Shrink only while the result still covers
	 * `target`, so the 640px hero still lands on 160x160 as before. */
	cinfo.scale_num   = 1;
	cinfo.scale_denom = 1;
	for (unsigned d = 8; d >= 2; d /= 2) {
		if (cinfo.image_width / d >= (unsigned)target &&
		    cinfo.image_height / d >= (unsigned)target) {
			cinfo.scale_denom = d;
			break;
		}
	}
	cinfo.out_color_space = JCS_EXT_RGBA;

	jpeg_start_decompress(&cinfo);

	const int w = (int)cinfo.output_width;
	const int h = (int)cinfo.output_height;

	/* Bound against the texture these pixels will be tiled into, which follows
	 * `target`, not the hero texture - a thumb decode legitimately produces
	 * something far smaller than ART_TEX_SIZE. */
	const int cap = art_tex_dim_for(target);

	if (w > cap || h > cap) {
		jpeg_abort_decompress(&cinfo);
		jpeg_destroy_decompress(&cinfo);
		http_free(&r);
		snprintf(err, errlen, "decoded %dx%d exceeds %d", w, h, cap);
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

	if (decode_ms)
		*decode_ms = (unsigned)(osGetTime() - t0);

	*out_rgba = linear; /* caller owns it now */
	*out_w    = w;
	*out_h    = h;
	return true;
}

/* GPU work only: tiling and the texture upload. Must run on the thread that
 * owns the graphics context, but it is cheap - the expensive part already
 * happened in art_fetch_decode, off the render thread. */
bool art_upload(album_art *a, const unsigned char *rgba, int w, int h,
                const char *url, char *err, int errlen)
{
	if (!rgba || w <= 0 || h <= 0) {
		snprintf(err, errlen, "no pixels");
		return false;
	}
	const int dim = art_tex_dim_for(w > h ? w : h);

	album_art staged = {0};
	art_accent_of(rgba, w, h, &staged);

	u8 *tiled = linearAlloc((size_t)dim * dim * 4);
	if (!tiled) {
		snprintf(err, errlen, "linearAlloc failed");
		return false;
	}

	art_tile_rgba(rgba, w, h, tiled, dim);

	return art_upload_tiled(a, tiled, w, h, dim, staged.accent_r,
	                        staged.accent_g, staged.accent_b, url, err, errlen);
}

bool art_upload_tiled(album_art *a, u8 *tiled, int w, int h, int dim,
                      u8 accent_r, u8 accent_g, u8 accent_b, const char *url,
                      char *err, int errlen)
{
	if (!tiled) {
		snprintf(err, errlen, "no texture data");
		return false;
	}

	/* The buffer's dimension cannot be inferred from w/h - a cached hero is
	 * 160px of image inside a 256px texture - so the caller states it. */
	if (dim <= 0)
		dim = ART_TEX_SIZE;

	art_free(a); /* release any previous texture before replacing it */

	if (!C3D_TexInit(&a->tex, (u16)dim, (u16)dim, GPU_RGBA8)) {
		linearFree(tiled);
		snprintf(err, errlen, "C3D_TexInit failed");
		return false;
	}

	C3D_TexLoadImage(&a->tex, tiled, GPU_TEXFACE_2D, 0);
	C3D_TexSetFilter(&a->tex, GPU_LINEAR, GPU_LINEAR);
	linearFree(tiled);

	/* The image occupies the first w*h texels, which - because the PICA200
	 * addresses textures bottom-up in V - is the TOP-LEFT of the texture as
	 * uploaded. citro2d's convention is top=1.0 at that edge, decreasing
	 * downwards, so the populated region runs from v=1.0 down to
	 * 1 - h/size. */
	a->sub.width  = (u16)w;
	a->sub.height = (u16)h;
	a->sub.left   = 0.0f;
	a->sub.right  = (float)w / dim;
	a->sub.top    = 1.0f;
	a->sub.bottom = 1.0f - (float)h / dim;

	a->image.tex    = &a->tex;
	a->image.subtex = &a->sub;
	a->src_w        = w;
	a->src_h        = h;
	a->tex_dim      = dim;
	a->accent_r     = accent_r;
	a->accent_g     = accent_g;
	a->accent_b     = accent_b;
	a->valid        = true;
	snprintf(a->url, sizeof a->url, "%s", url);

	return true;
}

/* Blocking convenience wrapper: fetch, decode and upload in one call. Only
 * safe off the render thread, since the fetch dominates. */
bool art_load(album_art *a, const char *url, char *err, int errlen)
{
	/* Art only changes on track change; skip the fetch otherwise. */
	if (a->valid && url && strcmp(a->url, url) == 0)
		return true;

	unsigned char *rgba = NULL;
	int            w = 0, h = 0;
	unsigned       fetch_ms = 0, decode_ms = 0;

	if (!art_fetch_decode(url, ART_HERO_PX, &rgba, &w, &h, &fetch_ms,
	                      &decode_ms, err, errlen))
		return false;

	const bool ok = art_upload(a, rgba, w, h, url, err, errlen);
	free(rgba);

	if (ok)
		a->decode_ms = decode_ms;
	return ok;
}
