#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "spotify/artcache_path.h"

#define MAX_ENTRIES ((int)((5ull * 1024 * 1024 * 1024) / (33 + 102400)))

int main(void)
{
	bool seen[256] = {false};
	for (int i = 0; i < 256; i++) {
		char key[65];
		snprintf(key, sizeof key,
		         "ab67616d0000b2730123456789abcdef0123456789abcdef0123456789ab%02x",
		         i);
		char suffix[3];
		int shard = -1;
		assert(artcache_shard_for_key(key, suffix, &shard));
		assert(shard == i);
		assert(!seen[shard]);
		seen[shard] = true;
		char expected[3];
		snprintf(expected, sizeof expected, "%02x", i);
		assert(strcmp(suffix, expected) == 0);
	}

	int total = 0;
	int min = MAX_ENTRIES;
	int max = 0;
	for (int i = 0; i < 256; i++) {
		const int quota = artcache_shard_quota_for(i, MAX_ENTRIES);
		total += quota;
		if (quota < min)
			min = quota;
		if (quota > max)
			max = quota;
	}
	assert(total == MAX_ENTRIES);
	assert(max - min <= 1);
	assert(!artcache_shard_for_key("ab67616d-not-hex", NULL, NULL));

	printf("artcache shards: 256/256 unique, quotas=%d..%d, total=%d\n",
	       min, max, total);
	return 0;
}
