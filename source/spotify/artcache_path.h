#pragma once

#include <stdbool.h>

/* Spotify image IDs have a constant prefix; shard on the variable hash tail. */
bool artcache_shard_for_key(const char *key, char suffix[3], int *index);

/* Split max_entries as evenly as possible across 256 shards. */
int artcache_shard_quota_for(int shard, int max_entries);
