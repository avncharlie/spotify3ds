#include "artcache_path.h"

#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#define ARTCACHE_SHARDS 256

artcache_quality artcache_entry_quality(uint16_t flags, unsigned tex_dim)
{
	if (flags & ARTCACHE_QUALITY_KNOWN_FLAG)
		return flags & ARTCACHE_LARGE_FLAG ? ARTCACHE_LARGE : ARTCACHE_THUMBNAIL;
	return tex_dim > 64 ? ARTCACHE_LARGE : ARTCACHE_THUMBNAIL;
}

uint16_t artcache_quality_flags(artcache_quality quality)
{
	return ARTCACHE_QUALITY_KNOWN_FLAG |
	       (quality == ARTCACHE_LARGE ? ARTCACHE_LARGE_FLAG : 0);
}

bool artcache_key_for_url(const char *url, char *out, size_t cap,
                          bool *mutable_image)
{
	if (!url || !url[0])
		return false;
	if (mutable_image)
		*mutable_image = false;
	const char *pickasso = "https://pickasso.spotifycdn.com/";
	const char *daylist = "https://daylist.spotifycdn.com/";
	if (strncmp(url, pickasso, strlen(pickasso)) == 0 ||
	    strncmp(url, daylist, strlen(daylist)) == 0) {
		/* Stable FNV-1a URL identity; unlike the locale suffix, it separates
		 * different generated covers. The namespace cannot collide with a hex ID. */
		uint64_t hash = UINT64_C(14695981039346656037);
		for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
			if (*p <= ' ' || *p == 127)
				return false;
			hash = (hash ^ *p) * UINT64_C(1099511628211);
		}
		if (cap < 19)
			return false;
		snprintf(out, cap, "p-%016" PRIx64, hash);
		if (mutable_image)
			*mutable_image = true;
		return true;
	}
	const char *slash = strrchr(url, '/');
	const char *id = slash ? slash + 1 : url;
	size_t n = strlen(id);
	if (n < 20 || n > 64 || n >= cap)
		return false;
	for (size_t i = 0; i < n; i++)
		if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f')))
			return false;
	memcpy(out, id, n + 1);
	return true;
}

bool artcache_timestamp_fresh(uint16_t flags, uint32_t written, uint32_t now)
{
	return !(flags & ARTCACHE_MUTABLE_FLAG) ||
	       (written != 0 && written <= now && now - written < ARTCACHE_GENERATED_TTL);
}

uint32_t artcache_ttl_for_url(const char *url)
{
	const char *daylist = "https://daylist.spotifycdn.com/";
	return url && strncmp(url, daylist, strlen(daylist)) == 0
	           ? 86400 : ARTCACHE_GENERATED_TTL;
}

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool artcache_shard_for_key(const char *key, char suffix[3], int *index)
{
	if (!key)
		return false;
	const size_t n = strlen(key);
	if (n < 2)
		return false;

	const int hi = hex_value(key[n - 2]);
	const int lo = hex_value(key[n - 1]);
	if (hi < 0 || lo < 0)
		return false;

	if (suffix) {
		suffix[0] = key[n - 2];
		suffix[1] = key[n - 1];
		suffix[2] = '\0';
	}
	if (index)
		*index = hi * 16 + lo;
	return true;
}

int artcache_shard_quota_for(int shard, int max_entries)
{
	if (shard < 0 || shard >= ARTCACHE_SHARDS || max_entries < 0)
		return 0;
	return max_entries / ARTCACHE_SHARDS +
	       (shard < max_entries % ARTCACHE_SHARDS ? 1 : 0);
}
