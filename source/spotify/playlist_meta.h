#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "json.h"

typedef enum {
	META_NONE = 0, /* temporary label/art, not playlist metadata */
	META_OEMBED,
	META_API,
} metadata_source;

typedef struct {
	char name[128];
	char owner[128];
	char art[256];
	metadata_source name_source, art_source;
} playlist_meta;

/* One request per step, so the worker can poll/control playback between the
 * metadata, cover and oEmbed requests. No snapshot/count data comes from here. */
typedef struct {
	char uri[128];
	char id[64];
	playlist_meta meta;
	bool fetched_name, fetched_art;
	int phase;
	time_t retry_at;
} playlist_meta_job;

bool playlist_meta_begin(playlist_meta_job *job, const char *uri,
                         const playlist_meta *seed);
/* Returns true when done, including a partial success or a failed request. */
bool playlist_meta_step(playlist_meta_job *job);
bool playlist_meta_complete(const playlist_meta *meta);
/* Decode bounded JSON strings and choose the smallest cover >= 52px. */
void playlist_meta_parse(playlist_meta *meta, const char *body, unsigned len,
                         int phase);
void playlist_meta_parse_doc(playlist_meta *meta, const json_doc *doc,
                             const char *base, int phase);
void playlist_meta_merge(playlist_meta *dst, const playlist_meta *src);

/* Session-local transport/HTTP backoff, shared by all playlist jobs. */
void playlist_meta_reset_backoff(void);
