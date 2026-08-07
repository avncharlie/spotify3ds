#include "artcache_path.h"

#include <stddef.h>
#include <string.h>

#define ARTCACHE_SHARDS 256

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
