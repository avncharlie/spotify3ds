#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>

#include "spotify/art.h"
#include "spotify/artcache.h"
#include "spotify/artcache_format.h"

#undef rename
int rename(const char *from, const char *to);
static bool sd_rename = true, fail_install;
static time_t now = 100000;

int test_sd_rename(const char *from, const char *to)
{
	FILE *existing = sd_rename ? fopen(to, "rb") : NULL;
	if (existing) {
		fclose(existing);
		errno = EEXIST;
		return -1;
	}
	if (fail_install) {
		errno = EIO;
		return -1;
	}
	return rename(from, to);
}
time_t time(time_t *out)
{
	if (out)
		*out = now;
	return now;
}
u64 osGetTime(void) { return (u64)now * 1000; }
void *linearAlloc(size_t size) { return malloc(size); }
void linearFree(void *ptr) { free(ptr); }
void tl_log(const char *format, ...) { (void)format; }
void tl_timing(const char *format, ...) { (void)format; }

/* Uniform pixels make every Morton tile identical. The real cache's row
 * packing, CRC, file replacement, quality and expiry logic are exercised. */
void art_tile_rgba(const u8 *rgba, int w, int h, u8 *tiled, int dim)
{
	assert(w > 0 && h > 0 && w <= dim && h <= dim);
	memset(tiled, rgba[0], (size_t)dim * dim * 4);
}

static void put(const char *url, artcache_quality quality, int size, u8 pixels)
{
	u8 *rgba = malloc((size_t)size * size * 4);
	assert(rgba);
	memset(rgba, pixels, (size_t)size * size * 4);
	artcache_store(url, quality, rgba, size, size, 1, 2, 3);
	free(rgba);
}
static bool get(const char *url, artcache_quality quality, int size, u8 pixels,
                 time_t *expiry)
{
	u8 *tiled = NULL;
	int w, h, dim;
	u8 r, g, b;
	unsigned read_ms;
	bool hit = artcache_load(url, quality, &tiled, &w, &h, &dim, &r, &g, &b,
	                         &read_ms, expiry);
	if (hit) {
		assert(tiled && w == size && h == size);
		assert(dim == art_tex_dim_for(size));
		assert(tiled[0] == pixels && r == 1 && g == 2 && b == 3);
		linearFree(tiled);
	} else
		assert(!tiled);
	return hit;
}

static void path_for(const char *url, char *path, size_t cap)
{
	char key[80], shard[3];
	assert(artcache_key_for_url(url, key, sizeof key, NULL));
	assert(artcache_shard_for_key(key, shard, NULL));
	snprintf(path, cap, "%s/%s/%s.a3c", CACHE_ROOT, shard, key);
}
static artcache_hdr header(const char *url)
{
	char path[512];
	path_for(url, path, sizeof path);
	FILE *f = fopen(path, "rb");
	assert(f);
	artcache_hdr h;
	assert(fread(&h, 1, sizeof h, f) == sizeof h);
	fclose(f);
	return h;
}
static void make_legacy(const char *url)
{
	char path[512];
	path_for(url, path, sizeof path);
	FILE *f = fopen(path, "r+b");
	assert(f);
	artcache_hdr h;
	assert(fread(&h, 1, sizeof h, f) == sizeof h);
	h.flags &= ARTCACHE_MUTABLE_FLAG;
	rewind(f);
	assert(fwrite(&h, 1, sizeof h, f) == sizeof h);
	fclose(f);
}

static void test_upgrade_and_protection(void)
{
	const char *a = "https://i.scdn.co/image/000000000000000000aa";
	const char *b = "https://i.scdn.co/image/100000000000000000aa";
	put(a, ARTCACHE_THUMBNAIL, 64, 10);
	put(b, ARTCACHE_THUMBNAIL, 64, 20);
	assert(get(a, ARTCACHE_THUMBNAIL, 64, 10, NULL));
	assert(!get(a, ARTCACHE_LARGE, 0, 0, NULL));
	assert(get(a, ARTCACHE_THUMBNAIL, 64, 10, NULL)); /* quality miss keeps thumbnail */
	artcache_init(); /* discover a full two-entry shard from disk */
	put(a, ARTCACHE_LARGE, 160, 30);
	assert(get(a, ARTCACHE_LARGE, 160, 30, NULL));
	assert(get(a, ARTCACHE_THUMBNAIL, 160, 30, NULL));
	assert(get(b, ARTCACHE_THUMBNAIL, 64, 20, NULL)); /* upgrade does not evict b */
	put(a, ARTCACHE_THUMBNAIL, 64, 40);
	assert(get(a, ARTCACHE_LARGE, 160, 30, NULL));
	/* Nor does an equally tagged, smaller decode replace higher-resolution pixels. */
	put(a, ARTCACHE_LARGE, 64, 50);
	assert(get(a, ARTCACHE_LARGE, 160, 30, NULL));

	const char *c = "https://i.scdn.co/image/200000000000000000aa";
	put(c, ARTCACHE_THUMBNAIL, 64, 60); /* one real new entry, one FIFO eviction */
	assert(get(c, ARTCACHE_THUMBNAIL, 64, 60, NULL));
	/* SD replacement changes insertion order; directory ordering also varies
	 * by host filesystem. Exactly one of the two earlier entries survives. */
	int survivors = get(a, ARTCACHE_LARGE, 160, 30, NULL) +
	                get(b, ARTCACHE_THUMBNAIL, 64, 20, NULL);
	assert(survivors == 1);
}

static void test_small_original_and_legacy(void)
{
	const char *small = "https://i.scdn.co/image/00000000000000000001";
	put(small, ARTCACHE_LARGE, 32, 11);
	assert(get(small, ARTCACHE_LARGE, 32, 11, NULL));
	assert(get(small, ARTCACHE_LARGE, 32, 11, NULL)); /* no infinite upgrade loop */
	assert(header(small).flags & ARTCACHE_LARGE_FLAG);

	const char *ambiguous = "https://i.scdn.co/image/00000000000000000002";
	put(ambiguous, ARTCACHE_THUMBNAIL, 64, 12);
	make_legacy(ambiguous);
	assert(get(ambiguous, ARTCACHE_THUMBNAIL, 64, 12, NULL));
	assert(!get(ambiguous, ARTCACHE_LARGE, 0, 0, NULL));
	put(ambiguous, ARTCACHE_LARGE, 64, 13);
	assert(get(ambiguous, ARTCACHE_LARGE, 64, 13, NULL));
	assert(header(ambiguous).flags & ARTCACHE_QUALITY_KNOWN_FLAG);

	const char *large = "https://i.scdn.co/image/00000000000000000003";
	put(large, ARTCACHE_LARGE, 160, 14);
	make_legacy(large);
	assert(get(large, ARTCACHE_LARGE, 160, 14, NULL));
	put(large, ARTCACHE_THUMBNAIL, 64, 15);
	assert(get(large, ARTCACHE_LARGE, 160, 14, NULL));
}

static void test_expiry(void)
{
	const char *url = "https://pickasso.spotifycdn.com/image/chill/en";
	time_t expiry;
	put(url, ARTCACHE_LARGE, 160, 21);
	assert(get(url, ARTCACHE_LARGE, 160, 21, &expiry));
	assert(expiry == now + 5 * 86400);
	now += 5 * 86400;
	/* An expired large image must not prevent a fresh thumbnail replacement. */
	put(url, ARTCACHE_THUMBNAIL, 64, 22);
	assert(get(url, ARTCACHE_THUMBNAIL, 64, 22, &expiry));
	assert(expiry == now + 5 * 86400);
	assert(!get(url, ARTCACHE_LARGE, 0, 0, NULL));
	put(url, ARTCACHE_LARGE, 160, 23);
	assert(get(url, ARTCACHE_LARGE, 160, 23, NULL));
	now += 5 * 86400;
	assert(!get(url, ARTCACHE_LARGE, 0, 0, NULL));

	url = "https://daylist.spotifycdn.com/covers/en/evening.jpg";
	put(url, ARTCACHE_LARGE, 64, 24);
	assert(get(url, ARTCACHE_LARGE, 64, 24, &expiry));
	assert(expiry == now + 86400);
	now += 86400;
	assert(!get(url, ARTCACHE_THUMBNAIL, 0, 0, NULL));
}

static void test_posix_replacement(void)
{
	const char *url = "https://i.scdn.co/image/00000000000000000004";
	sd_rename = false;
	put(url, ARTCACHE_THUMBNAIL, 64, 31);
	put(url, ARTCACHE_LARGE, 160, 32);
	assert(get(url, ARTCACHE_LARGE, 160, 32, NULL));
	sd_rename = true;
}

static void test_failed_install(void)
{
	const char *url = "https://i.scdn.co/image/00000000000000000005";
	put(url, ARTCACHE_THUMBNAIL, 64, 41);
	fail_install = true;
	put(url, ARTCACHE_LARGE, 160, 42);
	assert(!get(url, ARTCACHE_LARGE, 0, 0, NULL));
	fail_install = false;
	put(url, ARTCACHE_LARGE, 160, 43);
	assert(get(url, ARTCACHE_LARGE, 160, 43, NULL));
}

int main(void)
{
	artcache_init();
	test_upgrade_and_protection();
	test_small_original_and_legacy();
	test_expiry();
	test_posix_replacement();
	test_failed_install();
	puts("artwork quality: upgrades, protection, legacy/tiny originals, expiry, SD replacement and quotas passed");
	return 0;
}
