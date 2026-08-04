#include "artcache.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../testlog.h"

#define CACHE_ROOT "sdmc:/spotify/artcache"

/* A cache entry is a 32-byte header plus the populated tile rows of a 160x160
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
