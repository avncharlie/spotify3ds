#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "spotify/artcache_path.h"
#include "spotify/art_scale.h"

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

	char ordinary[80], generated[80], other[80], same[80];
	bool mutable_image = true;
	assert(artcache_key_for_url(
	    "https://i.scdn.co/image/ab67616d0000b2730123456789abcdef01234567",
	    ordinary, sizeof ordinary, &mutable_image));
	assert(!mutable_image);
	assert(strcmp(ordinary, "ab67616d0000b2730123456789abcdef01234567") == 0);
	assert(artcache_key_for_url("https://pickasso.spotifycdn.com/image/small/artist/en-GB",
	                            generated, sizeof generated, &mutable_image));
	assert(mutable_image && strncmp(generated, "p-", 2) == 0);
	assert(artcache_shard_for_key(generated, NULL, NULL));
	assert(artcache_key_for_url("https://pickasso.spotifycdn.com/image/small/artist/en-GB",
	                            same, sizeof same, NULL));
	assert(strcmp(generated, same) == 0);
	assert(artcache_key_for_url("https://pickasso.spotifycdn.com/image/large/artist/en-GB",
	                            other, sizeof other, NULL));
	assert(strcmp(generated, other) != 0);
	assert(artcache_key_for_url("https://pickasso.spotifycdn.com/image/small/artist/en",
	                            other, sizeof other, NULL));
	assert(strcmp(generated, other) != 0);
	assert(!artcache_key_for_url("https://pickasso.spotifycdn.com.evil/image/en",
	                             other, sizeof other, NULL));
	assert(!artcache_key_for_url("https://pickasso.spotifycdn.com/image/bad\nurl",
	                             other, sizeof other, NULL));
	assert(artcache_timestamp_fresh(0, 0, UINT32_MAX));
	assert(artcache_timestamp_fresh(ARTCACHE_MUTABLE_FLAG, 100,
	                               100 + ARTCACHE_GENERATED_TTL - 1));
	assert(!artcache_timestamp_fresh(ARTCACHE_MUTABLE_FLAG, 100,
	                                100 + ARTCACHE_GENERATED_TTL));
	assert(!artcache_timestamp_fresh(ARTCACHE_MUTABLE_FLAG, 0, 100));
	assert(!artcache_timestamp_fresh(ARTCACHE_MUTABLE_FLAG, 101, 100));
	assert(artcache_key_for_url("https://daylist.spotifycdn.com/playlist-covers-mix/en/evening_default.jpg",
	                            other, sizeof other, &mutable_image));
	assert(mutable_image && strcmp(generated, other) != 0);
	assert(artcache_ttl_for_url("https://daylist.spotifycdn.com/image.jpg") == 86400);
	assert(artcache_ttl_for_url("https://pickasso.spotifycdn.com/image/en") == 5 * 86400);

	unsigned w, h;
	assert(art_fit_dimensions(80, 80, 64, &w, &h) && w == 64 && h == 64);
	assert(art_fit_dimensions(64, 64, 64, &w, &h) && w == 64 && h == 64);
	assert(art_fit_dimensions(8192, 1024, 64, &w, &h) && w == 64 && h == 8);
	assert(art_fit_dimensions(1, 8192, 64, &w, &h) && w == 1 && h == 64);
	assert(!art_fit_dimensions(0, 80, 64, &w, &h));
	assert(!art_fit_dimensions(UINT32_MAX, 80, 64, &w, &h));
	uint8_t source[80 * 4], target[64 * 4];
	for (int i = 0; i < 80; i++) {
		source[i * 4] = (uint8_t)i;
		source[i * 4 + 1] = 2;
		source[i * 4 + 2] = 3;
		source[i * 4 + 3] = 255;
	}
	art_scale_row_rgba(source, 80, target, 64);
	for (unsigned x = 0; x < 64; x++) {
		assert(target[x * 4] == x * 80 / 64);
		assert(target[x * 4 + 1] == 2 && target[x * 4 + 3] == 255);
	}

	printf("artcache shards: 256/256 unique, quotas=%d..%d, total=%d\n",
	       min, max, total);
	return 0;
}
