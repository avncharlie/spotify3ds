#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define ARTCACHE_MUTABLE_FLAG 1
#define ARTCACHE_QUALITY_KNOWN_FLAG 2
#define ARTCACHE_LARGE_FLAG 4
#define ARTCACHE_GENERATED_TTL (5u * 24 * 3600)

typedef enum {
	ARTCACHE_THUMBNAIL = 0,
	ARTCACHE_LARGE,
} artcache_quality;

/* Legacy textures >64px necessarily came from the large decode path. Smaller
 * legacy entries are ambiguous and upgrade once when a large cover is wanted. */
artcache_quality artcache_entry_quality(uint16_t flags, unsigned tex_dim);
uint16_t artcache_quality_flags(artcache_quality quality);

/* Preserve content-ID keys; generated Pickasso images use a namespaced hash of
 * the entire URL (including size, artist and locale) and expire. */
bool artcache_key_for_url(const char *url, char *out, size_t cap,
                          bool *mutable_image);
bool artcache_timestamp_fresh(uint16_t flags, uint32_t written, uint32_t now);
uint32_t artcache_ttl_for_url(const char *url);

/* Spotify image IDs have a constant prefix; shard on the variable hash tail. */
bool artcache_shard_for_key(const char *key, char suffix[3], int *index);

/* Split max_entries as evenly as possible across 256 shards. */
int artcache_shard_quota_for(int shard, int max_entries);
