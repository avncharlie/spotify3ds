#include "artcache.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../testlog.h"

#include "art.h"
#include "artcache_path.h"

#define CACHE_ROOT "sdmc:/spotify/artcache"

#define ARTCACHE_MAGIC 0x43334100u /* "\0A3C" */

/* Bound by the worst-case 160x160 payload. Thumbnails are smaller, so actual
 * payload usage is normally well below this conservative 5 GiB ceiling. */
#define ARTCACHE_MAX_BYTES (5ull * 1024 * 1024 * 1024)

/* A 160x160 image inside a 256x256 texture occupies 20x20 tiles of 8x8. The
 * tiles are laid out row-major across the *full* 32-tile-wide texture, so the
 * populated ones are 20 runs of 20 tiles rather than one contiguous block.
 * Storing just those runs costs 100KB instead of the 256KB the whole texture
 * would take, for 20 memcpys on load. */
#define TILE_BYTES     (64 * 4) /* 8x8 texels, 4 bytes each */

/* Packed: this struct is written verbatim to disk, so the on-disk layout must
 * be exactly what is written here rather than whatever padding the compiler
 * chooses. Leaving it implicit already cost one debugging session - two bytes
 * inserted after the accent triple silently shifted every field after it. */
typedef struct __attribute__((packed)) {
	u32 magic;
	u16 version;
	u16 flags;
	u16 tex_dim;   /* guards ART_TEX_SIZE changes even without a version bump */
	u16 src_w;
	u16 src_h;
	u8  accent_r, accent_g, accent_b;
	u32 payload_len;
	u32 crc32;

	/* Retained to keep the compact header layout stable. Global LRU required
	 * opening every entry at startup, which is prohibitively slow on 3DS SD
	 * storage; eviction is now FIFO within independently bounded hash shards. */
	u32 use_seq;
	u32 reserved;
} artcache_hdr;

/* The header is part of the on-disk format, so assert its size rather than
 * trusting a comment. Packed, the fields total exactly 33 bytes. */
_Static_assert(sizeof(artcache_hdr) == 33, "artcache header layout changed - "
                                           "bump ARTCACHE_VERSION");

static bool s_writes_disabled;

/* A cache entry is a header plus the populated tile rows of a 160x160
 * image inside a 256x256 texture: 20 rows of 20 tiles, 64 texels each, 4 bytes
 * per texel = 102400. The probe writes exactly this so the numbers describe a
 * real entry rather than an arbitrary block. */
#define PROBE_PAYLOAD 102400

/* Table-driven CRC32 (reflected, poly 0xEDB88320). The table is 1KB of .bss
 * built once on first use; the bitwise variant costs ~30ms per entry on this
 * CPU, which would be a third of the entire budget for a cache hit. */
static unsigned s_crc_table[256];
static bool     s_crc_ready;

static void crc32_init(void)
{
	for (unsigned i = 0; i < 256; i++) {
		unsigned c = i;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320u & (unsigned)(-(int)(c & 1)));
		s_crc_table[i] = c;
	}
	s_crc_ready = true;
}

static unsigned crc32_buf(const unsigned char *p, size_t n)
{
	if (!s_crc_ready)
		crc32_init();

	unsigned crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; i++)
		crc = s_crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

/* Spotify art URLs end in a hex content hash, which is already unique, stable
 * and filename-safe. Anything that does not look like one is rejected rather
 * than sanitised - that also keeps a malformed URL from reaching fopen. */
static bool artcache_key(const char *url, char *out, int outlen)
{
	if (!url || !url[0])
		return false;

	const char *slash = strrchr(url, '/');
	const char *seg   = slash ? slash + 1 : url;

	int n = 0;
	while (seg[n]) {
		const char c = seg[n];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return false;
		n++;
	}

	if (n < 20 || n > 64 || n >= outlen)
		return false;

	memcpy(out, seg, (size_t)n);
	out[n] = '\0';
	return true;
}

/* sdmc:/spotify/artcache/<last two hex>/<key>.a3c
 *
 * Spotify's IDs start with a size/type prefix shared by nearly every image, so
 * the first two characters are always "ab". The content-hash tail is uniform
 * and actually distributes entries across the 256 shards.
 *
 * FAT32 scans directory entries linearly and long filenames consume several
 * 32-byte slots each, so a flat directory of thousands of covers would be
 * hundreds of KB to walk on every open. Sharding keeps it to a couple of KB. */
static void artcache_paths(const char *key, char *dir, int dirlen, char *file,
                           int filelen, char *tmp, int tmplen)
{
	char suffix[3];
	artcache_shard_for_key(key, suffix, NULL);
	snprintf(dir, dirlen, CACHE_ROOT "/%s", suffix);
	if (file)
		snprintf(file, filelen, "%s/%s.a3c", dir, key);
	if (tmp)
		snprintf(tmp, tmplen, "%s/%s.tmp", dir, key);
}

/* Use the largest possible payload so the logical cache size cannot exceed the
 * budget even if every entry is hero-sized. Distribute the remainder across
 * the first shards: 187 shards hold 205 files and 69 hold 204. */
#define ARTCACHE_MAX_ENTRIES \
	((int)(ARTCACHE_MAX_BYTES / (sizeof(artcache_hdr) + 102400)))
#define ARTCACHE_SHARDS 256

static u16  s_shard_count[ARTCACHE_SHARDS];
static bool s_shard_known[ARTCACHE_SHARDS];

static int shard_index(const char *key)
{
	int shard = -1;
	return artcache_shard_for_key(key, NULL, &shard) ? shard : -1;
}

static int shard_quota(int shard)
{
	return artcache_shard_quota_for(shard, ARTCACHE_MAX_ENTRIES);
}

static void discard_entry(const char *key, const char *file)
{
	if (unlink(file) != 0)
		return;
	const int shard = shard_index(key);
	if (shard >= 0 && s_shard_known[shard] && s_shard_count[shard] > 0)
		s_shard_count[shard]--;
}

static bool cache_filename(const char *name, const char *extension)
{
	const size_t len = strlen(name);
	return len >= 5 && len <= 68 &&
	       strcmp(name + len - 4, extension) == 0;
}

/* Directory order on FAT follows insertion order closely enough for FIFO.
 * Unlike exact LRU, this needs no per-hit write and never touches another
 * shard. It is called only when a known shard is already at its quota. */
static bool evict_one(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d)
		return false;

	bool removed = false;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char path[256];
		snprintf(path, sizeof path, "%s/%.68s", dir, e->d_name);
		if (cache_filename(e->d_name, ".tmp")) {
			unlink(path);
			continue;
		}
		if (cache_filename(e->d_name, ".a3c") && unlink(path) == 0) {
			removed = true;
			break;
		}
	}
	closedir(d);
	return removed;
}

/* Discover one shard lazily on its first write. Merely counting directory
 * entries avoids the expensive fopen/fread per file that caused minute-long
 * startup. Overfull legacy shards are pruned in directory (FIFO) order. */
static void prepare_shard(const char *dir, int shard)
{
	if (s_shard_known[shard])
		return;

	int count = 0;
	DIR *d = opendir(dir);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.')
				continue;
			char path[256];
			snprintf(path, sizeof path, "%s/%.68s", dir, e->d_name);
			if (cache_filename(e->d_name, ".tmp"))
				unlink(path);
			else if (cache_filename(e->d_name, ".a3c"))
				count++;
		}
		closedir(d);
	}

	const int quota = shard_quota(shard);
	while (count >= quota && evict_one(dir))
		count--;
	s_shard_count[shard] = (u16)count;
	s_shard_known[shard] = true;
}

void artcache_init(void)
{
	/* Deliberately no filesystem work. Even opening every 33-byte cache header
	 * took almost a minute with a large cache on real hardware. */
	memset(s_shard_count, 0, sizeof s_shard_count);
	memset(s_shard_known, 0, sizeof s_shard_known);
}

bool artcache_load(const char *url, u8 **out_tiled, int *out_w, int *out_h,
                   int *out_dim, u8 *accent_r, u8 *accent_g, u8 *accent_b,
                   unsigned *read_ms)
{
	*out_tiled = NULL;

	char key[80];
	if (!artcache_key(url, key, sizeof key))
		return false;

	char dir[160], file[256];
	artcache_paths(key, dir, sizeof dir, file, sizeof file, NULL, 0);

	const u64 t0 = osGetTime();

	FILE *f = fopen(file, "rb");
	if (!f)
		return false; /* ordinary miss */

	artcache_hdr h;
	if (fread(&h, 1, sizeof h, f) != sizeof h) {
		fclose(f);
		discard_entry(key, file);
		return false;
	}

	const u32 rows      = (u32)h.src_h ? (((u32)h.src_h + 7) / 8) : 0;
	const u32 cols      = (u32)h.src_w ? (((u32)h.src_w + 7) / 8) : 0;
	const u32 want_len  = rows * cols * TILE_BYTES;

	/* The entry states its own texture size - thumbs are 64, heroes 256 - so
	 * validate against that rather than against the hero constant. */
	const u32 dim = h.tex_dim;

	if (h.magic != ARTCACHE_MAGIC || h.version != ARTCACHE_VERSION ||
	    dim == 0 || (dim & (dim - 1)) != 0 || dim > ART_TEX_SIZE ||
	    h.payload_len != want_len || h.src_w > dim || h.src_h > dim ||
	    want_len == 0) {
		/* Stale format or nonsense: drop it and refetch. */
		fclose(f);
		discard_entry(key, file);
		return false;
	}

	u8 *rowbuf = malloc(want_len);
	if (!rowbuf) {
		fclose(f);
		return false;
	}

	const size_t got = fread(rowbuf, 1, want_len, f);
	fclose(f);

	if (got != want_len || crc32_buf(rowbuf, want_len) != h.crc32) {
		/* Truncated or corrupt. Deleting makes this self-healing: the next
		 * request refetches and rewrites. */
		free(rowbuf);
		discard_entry(key, file);
		tl_log("artcache: corrupt entry %s, discarded", key);
		return false;
	}

	/* Scatter the stored tile-rows back into a full texture. */
	u8 *tiled = linearAlloc((size_t)dim * dim * 4);
	if (!tiled) {
		free(rowbuf);
		return false;
	}
	memset(tiled, 0, (size_t)dim * dim * 4);

	for (u32 r = 0; r < rows; r++)
		memcpy(tiled + (size_t)r * (dim / 8) * TILE_BYTES,
		       rowbuf + (size_t)r * cols * TILE_BYTES,
		       (size_t)cols * TILE_BYTES);

	free(rowbuf);

	*out_tiled = tiled;
	*out_w     = h.src_w;
	*out_h     = h.src_h;
	if (out_dim)
		*out_dim = (int)dim;
	*accent_r  = h.accent_r;
	*accent_g  = h.accent_g;
	*accent_b  = h.accent_b;
	if (read_ms)
		*read_ms = (unsigned)(osGetTime() - t0);

	return true;
}

void artcache_store(const char *url, const u8 *rgba, int w, int h, u8 accent_r,
                    u8 accent_g, u8 accent_b)
{
	if (s_writes_disabled || !rgba || w <= 0 || h <= 0 ||
	    w > ART_TEX_SIZE || h > ART_TEX_SIZE)
		return;

	char key[80];
	if (!artcache_key(url, key, sizeof key))
		return;

	char dir[160], file[256], tmp[256];
	artcache_paths(key, dir, sizeof dir, file, sizeof file, tmp, sizeof tmp);
	const int shard = shard_index(key);
	if (shard < 0)
		return;

	/* No cache path is touched during startup. Create and account for only the
	 * shard receiving this new entry, then reserve one slot before doing the
	 * tiling/allocation work below. */
	mkdir(CACHE_ROOT, 0777);
	mkdir(dir, 0777);
	prepare_shard(dir, shard);
	if (s_shard_count[shard] >= shard_quota(shard)) {
		if (!evict_one(dir)) {
			tl_log("artcache: full shard %.2s could not evict", key);
			return;
		}
		s_shard_count[shard]--;
	}

	/* Tile into a full texture, then keep only the populated rows. The texture
	 * is sized to this image, so a 64px thumb is stored as a 64px entry rather
	 * than rattling around inside a hero-sized one. */
	const int dim = art_tex_dim_for(w > h ? w : h);

	u8 *tiled = linearAlloc((size_t)dim * dim * 4);
	if (!tiled)
		return;
	art_tile_rgba(rgba, w, h, tiled, dim);

	const u32 rows = ((u32)h + 7) / 8;
	const u32 cols = ((u32)w + 7) / 8;
	const u32 len  = rows * cols * TILE_BYTES;

	/* Header and payload in one buffer, so the file goes out in a single
	 * fwrite. See the write call below for why that matters. */
	const size_t total = sizeof(artcache_hdr) + len;

	u8 *hdr_and_rows = malloc(total);
	if (!hdr_and_rows) {
		linearFree(tiled);
		return;
	}

	u8 *const rowbuf = hdr_and_rows + sizeof(artcache_hdr);
	for (u32 r = 0; r < rows; r++)
		memcpy(rowbuf + (size_t)r * cols * TILE_BYTES,
		       tiled + (size_t)r * (dim / 8) * TILE_BYTES,
		       (size_t)cols * TILE_BYTES);
	linearFree(tiled);

	artcache_hdr hdr;
	memset(&hdr, 0, sizeof hdr);
	hdr.magic       = ARTCACHE_MAGIC;
	hdr.version     = ARTCACHE_VERSION;
	hdr.tex_dim     = (u16)dim;
	hdr.src_w       = (u16)w;
	hdr.src_h       = (u16)h;
	hdr.accent_r    = accent_r;
	hdr.accent_g    = accent_g;
	hdr.accent_b    = accent_b;
	hdr.payload_len = len;
	hdr.crc32       = crc32_buf(rowbuf, len);
	hdr.use_seq     = 0; /* legacy field; FIFO eviction uses directory order */
	memcpy(hdr_and_rows, &hdr, sizeof hdr);

	/* Write to .tmp and rename, so a power loss can never leave a half-written
	 * entry under the real name. Orphans are removed when this shard is next
	 * prepared or considered for eviction. */
	FILE *f = fopen(tmp, "wb");
	if (!f) {
		tl_log("artcache: cannot write (%s) - caching disabled this session",
		       tmp);
		s_writes_disabled = true;
		free(hdr_and_rows);
		return;
	}

	/* One write, not two. Writing the 33-byte header separately left the
	 * payload misaligned to the SD block size, and the 3DS FS layer turned
	 * that into a read-modify-write per block: 1716ms against 60ms for the
	 * same bytes written in a single aligned call. */
	const bool ok = fwrite(hdr_and_rows, 1, total, f) == total;
	fflush(f);
	fclose(f);
	free(hdr_and_rows);

	if (!ok) {
		unlink(tmp);
		tl_log("artcache: short write - caching disabled this session");
		s_writes_disabled = true;
		return;
	}

	if (rename(tmp, file) != 0) {
		unlink(tmp);
		tl_log("artcache: rename failed for %s", key);
	} else {
		s_shard_count[shard]++;
	}
}

void artcache_probe(void)
{
	const char *dir = CACHE_ROOT "/probe";
	const char *tmp = CACHE_ROOT "/probe/p.tmp";
	const char *fin = CACHE_ROOT "/probe/p.a3c";

	unsigned char *buf = malloc(PROBE_PAYLOAD);
	if (!buf) {
		tl_timing("sdcache probe: malloc failed");
		return;
	}
	/* Non-uniform so the CRC is not measuring an unrealistically cache-friendly
	 * pattern, and so a truncated read is detectable. */
	for (int i = 0; i < PROBE_PAYLOAD; i++)
		buf[i] = (unsigned char)(i * 31 + (i >> 8));

	u64 t;

	/* --- create ------------------------------------------------------ */
	t = osGetTime();
	mkdir(CACHE_ROOT, 0777);
	mkdir(dir, 0777);
	const u64 mkdir_ms = osGetTime() - t;

	/* --- write via the atomic path the real cache will use ----------- */
	t          = osGetTime();
	FILE *f    = fopen(tmp, "wb");
	const u64 open_w_ms = osGetTime() - t;
	if (!f) {
		tl_timing("sdcache probe: open for write FAILED");
		free(buf);
		return;
	}

	t = osGetTime();
	const size_t wrote = fwrite(buf, 1, PROBE_PAYLOAD, f);
	fflush(f);
	fclose(f);
	const u64 write_ms = osGetTime() - t;

	t = osGetTime();
	const int rn = rename(tmp, fin);
	const u64 rename_ms = osGetTime() - t;

	tl_timing("sdcache probe: mkdir=%llums open_w=%llums write=%llums "
	          "rename=%llums wrote=%u rc=%d",
	          (unsigned long long)mkdir_ms, (unsigned long long)open_w_ms,
	          (unsigned long long)write_ms, (unsigned long long)rename_ms,
	          (unsigned)wrote, rn);

	if (wrote != PROBE_PAYLOAD || rn != 0) {
		tl_timing("sdcache probe: write path FAILED, aborting read test");
		free(buf);
		return;
	}

	/* --- read back: this is the number that decides the design ------- */
	memset(buf, 0, PROBE_PAYLOAD);

	t       = osGetTime();
	FILE *r = fopen(fin, "rb");
	const u64 open_r_ms = osGetTime() - t;
	if (!r) {
		tl_timing("sdcache probe: open for read FAILED");
		free(buf);
		return;
	}

	t = osGetTime();
	const size_t got = fread(buf, 1, PROBE_PAYLOAD, r);
	const u64 read_ms = osGetTime() - t;
	fclose(r);

	t = osGetTime();
	const unsigned crc = crc32_buf(buf, PROBE_PAYLOAD);
	const u64 crc_ms = osGetTime() - t;

	/* Second read exposes any FS-layer caching. If this is dramatically
	 * faster, the first number is the one that matters for a cold hit. */
	t = osGetTime();
	FILE *r2 = fopen(fin, "rb");
	size_t got2 = 0;
	if (r2) {
		got2 = fread(buf, 1, PROBE_PAYLOAD, r2);
		fclose(r2);
	}
	const u64 read2_ms = osGetTime() - t;

	tl_timing("sdcache probe: open_r=%llums read=%llums read2=%llums "
	          "crc=%llums bytes=%u crc32=%08X",
	          (unsigned long long)open_r_ms, (unsigned long long)read_ms,
	          (unsigned long long)read2_ms, (unsigned long long)crc_ms,
	          (unsigned)got, crc);

	/* The figure the plan's decision gate is stated against. */
	tl_timing("sdcache probe: HIT_COST=%llums (open_r+read+crc)",
	          (unsigned long long)(open_r_ms + read_ms + crc_ms));

	if (got2 != PROBE_PAYLOAD)
		tl_timing("sdcache probe: second read short (%u)", (unsigned)got2);

	unlink(fin);
	rmdir(dir);
	free(buf);
}
