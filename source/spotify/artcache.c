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

#define CACHE_ROOT "sdmc:/spotify/artcache"

#define ARTCACHE_MAGIC 0x43334100u /* "\0A3C" */

/* Entries are fixed-size, so the 2GB budget is just a count. */
#define ARTCACHE_MAX_BYTES (2ull * 1024 * 1024 * 1024)

/* A 160x160 image inside a 256x256 texture occupies 20x20 tiles of 8x8. The
 * tiles are laid out row-major across the *full* 32-tile-wide texture, so the
 * populated ones are 20 runs of 20 tiles rather than one contiguous block.
 * Storing just those runs costs 100KB instead of the 256KB the whole texture
 * would take, for 20 memcpys on load. */
#define TILE_BYTES     (64 * 4) /* 8x8 texels, 4 bytes each */
#define TILES_PER_ROW  (ART_TEX_SIZE / 8)

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

	/* Recency, for eviction. The obvious choice would be the file's mtime, but
	 * the 3DS filesystem layer leaves st_mtime as 0 for every entry and utime()
	 * is inert, so LRU by mtime silently evicts arbitrary files. Instead each
	 * entry carries a counter stamped on write and refreshed on every hit; the
	 * sweep evicts the lowest. Wraparound is not a practical concern at a few
	 * hundred track changes a day. */
	u32 use_seq;
	u32 reserved;
} artcache_hdr;

/* The header is part of the on-disk format, so assert its size rather than
 * trusting a comment. Packed, the fields total exactly 33 bytes. */
_Static_assert(sizeof(artcache_hdr) == 33, "artcache header layout changed - "
                                           "bump ARTCACHE_VERSION");

/* Highest use_seq seen this session, so refreshes keep climbing across runs. */
static u32 s_use_seq;

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

/* sdmc:/spotify/artcache/<first two hex>/<key>.a3c
 *
 * FAT32 scans directory entries linearly and long filenames consume several
 * 32-byte slots each, so a flat directory of thousands of covers would be
 * hundreds of KB to walk on every open. Sharding keeps it to a couple of KB. */
static void artcache_paths(const char *key, char *dir, int dirlen, char *file,
                           int filelen, char *tmp, int tmplen)
{
	snprintf(dir, dirlen, CACHE_ROOT "/%c%c", key[0], key[1]);
	if (file)
		snprintf(file, filelen, "%s/%s.a3c", dir, key);
	if (tmp)
		snprintf(tmp, tmplen, "%s/%s.tmp", dir, key);
}

/* Entries are all the same size, so the byte budget is really a count. */
#define ARTCACHE_MAX_ENTRIES \
	((int)(ARTCACHE_MAX_BYTES / (sizeof(artcache_hdr) + 102400)))

typedef struct {
	char path[192];
	u32  use_seq;
} sweep_entry;

static int sweep_cmp(const void *a, const void *b)
{
	const u32 ta = ((const sweep_entry *)a)->use_seq;
	const u32 tb = ((const sweep_entry *)b)->use_seq;
	return (ta > tb) - (ta < tb); /* least recently used first */
}

void artcache_init(void)
{
	mkdir(CACHE_ROOT, 0777);
	/* Shard directories are created lazily on first write: one mkdir costs
	 * ~80ms on hardware, so creating all 256 up front would stall startup for
	 * roughly twenty seconds. */

	const u64 t0 = osGetTime();

	DIR *root = opendir(CACHE_ROOT);
	if (!root)
		return;

	/* Collect entries, deleting .tmp orphans as we go. A .tmp at startup is by
	 * definition abandoned - nothing survives a reboot mid-write. */
	sweep_entry *ents = NULL;
	int          n = 0, cap = 0, orphans = 0;

	struct dirent *shard;
	while ((shard = readdir(root))) {
		if (shard->d_name[0] == '.')
			continue;

		/* Shard names are two hex chars; anything longer is not ours. */
		if (strlen(shard->d_name) != 2)
			continue;

		char shard_path[64];
		snprintf(shard_path, sizeof shard_path, CACHE_ROOT "/%.2s",
		         shard->d_name);

		DIR *d = opendir(shard_path);
		if (!d)
			continue;

		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.')
				continue;

			/* Entry names are "<hash>.a3c" or "<hash>.tmp"; a hash is at most
			 * 64 hex chars, so anything longer is not ours. Bounding the copy
			 * also keeps the path within `full`. */
			const size_t len = strlen(e->d_name);
			if (len < 5 || len > 68)
				continue;

			char full[192];
			snprintf(full, sizeof full, "%s/%.68s", shard_path, e->d_name);

			if (strcmp(e->d_name + len - 4, ".tmp") == 0) {
				unlink(full);
				orphans++;
				continue;
			}

			if (n == cap) {
				const int ncap = cap ? cap * 2 : 64;
				sweep_entry *p = realloc(ents, (size_t)ncap * sizeof *ents);
				if (!p)
					break; /* out of memory: sweep what we have */
				ents = p;
				cap  = ncap;
			}

			/* Read just the header for its use counter. Cheap next to the
			 * 100KB payload, and the only recency signal available. */
			u32 seq = 0;
			{
				FILE *hf = fopen(full, "rb");
				if (hf) {
					artcache_hdr h;
					if (fread(&h, 1, sizeof h, hf) == sizeof h &&
					    h.magic == ARTCACHE_MAGIC)
						seq = h.use_seq;
					fclose(hf);
				}
			}

			snprintf(ents[n].path, sizeof ents[n].path, "%s", full);
			ents[n].use_seq = seq;
			/* Resume the counter above the highest already on disk, so
			 * refreshes keep climbing across runs instead of restarting at 1
			 * and making old entries look newer than recent ones. */
			if (seq > s_use_seq)
				s_use_seq = seq;
			n++;
		}
		closedir(d);
	}
	closedir(root);

	int evicted = 0;
	if (n > ARTCACHE_MAX_ENTRIES && ents) {
		qsort(ents, (size_t)n, sizeof *ents, sweep_cmp);
		const int to_drop = n - ARTCACHE_MAX_ENTRIES;
		for (int i = 0; i < to_drop; i++) {
			unlink(ents[i].path);
			evicted++;
		}
	}

	free(ents);

	if (orphans || evicted || n)
		tl_timing("artcache sweep: scanned=%d evicted=%d orphans=%d took=%lldms",
		          n, evicted, orphans, (long long)(osGetTime() - t0));
}

bool artcache_load(const char *url, u8 **out_tiled, int *out_w, int *out_h,
                   u8 *accent_r, u8 *accent_g, u8 *accent_b, unsigned *read_ms)
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
		unlink(file);
		return false;
	}

	const u32 rows      = (u32)h.src_h ? (((u32)h.src_h + 7) / 8) : 0;
	const u32 cols      = (u32)h.src_w ? (((u32)h.src_w + 7) / 8) : 0;
	const u32 want_len  = rows * cols * TILE_BYTES;

	if (h.magic != ARTCACHE_MAGIC || h.version != ARTCACHE_VERSION ||
	    h.tex_dim != ART_TEX_SIZE || h.payload_len != want_len ||
	    h.src_w > ART_TEX_SIZE || h.src_h > ART_TEX_SIZE || want_len == 0) {
		/* Stale format or nonsense: drop it and refetch. */
		fclose(f);
		unlink(file);
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
		unlink(file);
		tl_log("artcache: corrupt entry %s, discarded", key);
		return false;
	}

	/* Scatter the stored tile-rows back into a full texture. */
	u8 *tiled = linearAlloc((size_t)ART_TEX_SIZE * ART_TEX_SIZE * 4);
	if (!tiled) {
		free(rowbuf);
		return false;
	}
	memset(tiled, 0, (size_t)ART_TEX_SIZE * ART_TEX_SIZE * 4);

	for (u32 r = 0; r < rows; r++)
		memcpy(tiled + (size_t)r * TILES_PER_ROW * TILE_BYTES,
		       rowbuf + (size_t)r * cols * TILE_BYTES,
		       (size_t)cols * TILE_BYTES);

	free(rowbuf);

	/* Refresh the use counter in place so eviction reflects last use, not last
	 * write. Only the header is rewritten - 32 bytes, not the payload - and a
	 * failure here is harmless: the entry simply looks older than it is. */
	{
		FILE *uf = fopen(file, "r+b");
		if (uf) {
			h.use_seq = ++s_use_seq;
			fwrite(&h, 1, sizeof h, uf);
			fclose(uf);
		}
	}

	*out_tiled = tiled;
	*out_w     = h.src_w;
	*out_h     = h.src_h;
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

	/* Tile into a full texture, then keep only the populated rows. */
	u8 *tiled = linearAlloc((size_t)ART_TEX_SIZE * ART_TEX_SIZE * 4);
	if (!tiled)
		return;
	art_tile_rgba(rgba, w, h, tiled, ART_TEX_SIZE);

	const u32 rows = ((u32)h + 7) / 8;
	const u32 cols = ((u32)w + 7) / 8;
	const u32 len  = rows * cols * TILE_BYTES;

	u8 *rowbuf = malloc(len);
	if (!rowbuf) {
		linearFree(tiled);
		return;
	}
	for (u32 r = 0; r < rows; r++)
		memcpy(rowbuf + (size_t)r * cols * TILE_BYTES,
		       tiled + (size_t)r * TILES_PER_ROW * TILE_BYTES,
		       (size_t)cols * TILE_BYTES);
	linearFree(tiled);

	artcache_hdr hdr;
	memset(&hdr, 0, sizeof hdr);
	hdr.magic       = ARTCACHE_MAGIC;
	hdr.version     = ARTCACHE_VERSION;
	hdr.tex_dim     = ART_TEX_SIZE;
	hdr.src_w       = (u16)w;
	hdr.src_h       = (u16)h;
	hdr.accent_r    = accent_r;
	hdr.accent_g    = accent_g;
	hdr.accent_b    = accent_b;
	hdr.payload_len = len;
	hdr.crc32       = crc32_buf(rowbuf, len);
	hdr.use_seq     = ++s_use_seq;

	mkdir(dir, 0777); /* lazy: only the shards actually used ever exist */

	/* Write to .tmp and rename, so a power loss can never leave a half-written
	 * entry under the real name. Orphaned .tmp files are swept at startup. */
	FILE *f = fopen(tmp, "wb");
	if (!f) {
		tl_log("artcache: cannot write (%s) - caching disabled this session",
		       tmp);
		s_writes_disabled = true;
		free(rowbuf);
		return;
	}

	const bool ok = fwrite(&hdr, 1, sizeof hdr, f) == sizeof hdr &&
	                fwrite(rowbuf, 1, len, f) == len;
	fflush(f);
	fclose(f);
	free(rowbuf);

	if (!ok) {
		unlink(tmp);
		tl_log("artcache: short write - caching disabled this session");
		s_writes_disabled = true;
		return;
	}

	if (rename(tmp, file) != 0) {
		unlink(tmp);
		tl_log("artcache: rename failed for %s", key);
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
