#include "playlist_meta.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth.h"
#include "json.h"
#include "../net/http.h"
#include "../testlog.h"

static time_t s_api_retry, s_embed_retry;

bool playlist_meta_complete(const playlist_meta *meta)
{
	return meta->name_source != META_NONE && meta->name[0] &&
	       meta->art_source != META_NONE && meta->art[0];
}

void playlist_meta_merge(playlist_meta *dst, const playlist_meta *src)
{
	if (src->name[0] && src->name_source != META_NONE &&
	    src->name_source >= dst->name_source) {
		memcpy(dst->name, src->name, sizeof dst->name);
		dst->name_source = src->name_source;
	}
	if (src->art[0] && src->art_source != META_NONE &&
	    src->art_source >= dst->art_source) {
		memcpy(dst->art, src->art, sizeof dst->art);
		dst->art_source = src->art_source;
	}
	if (src->owner[0])
		memcpy(dst->owner, src->owner, sizeof dst->owner);
}

static bool read_string(const json_doc *d, const char *path, char *out,
                        size_t cap, bool url)
{
	char *value = NULL;
	size_t n = 0;
	if (json_doc_str_alloc(d, path, cap - 1, &value, &n) != JSON_ALLOC_OK)
		return false;
	bool ok = n != 0 && strlen(value) == n;
	bool visible = false;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)value[i];
		if (c < 32 || c == 127 || (url && c == ' '))
			ok = false;
		if (c > ' ')
			visible = true;
	}
	if (url && strncmp(value, "https://", 8) != 0)
		ok = false;
	if (ok && visible)
		memcpy(out, value, n + 1);
	free(value);
	return ok && visible;
}

static void field_path(char *out, size_t cap, const char *base, const char *field)
{
	snprintf(out, cap, "%s%s%s", base, base[0] && field[0] != '[' ? "." : "", field);
}

void playlist_meta_parse_doc(playlist_meta *meta, const json_doc *d,
                             const char *base, int phase)
{
	char path[192];
	playlist_meta parsed = {0};
	if (phase == 0) {
		field_path(path, sizeof path, base, "name");
		if (read_string(d, path, parsed.name, sizeof parsed.name, false))
			parsed.name_source = META_API;
		field_path(path, sizeof path, base, "owner.display_name");
		read_string(d, path, parsed.owner, sizeof parsed.owner,
		            false);
	}
	if (phase == 2) {
		field_path(path, sizeof path, base, "title");
		if (read_string(d, path, parsed.name, sizeof parsed.name, false))
			parsed.name_source = META_OEMBED;
		field_path(path, sizeof path, base, "thumbnail_url");
		if (read_string(d, path, parsed.art, sizeof parsed.art, true))
			parsed.art_source = META_OEMBED;
	} else {
		const char *array = phase == 0 ? "images" : "";
		field_path(path, sizeof path, base, array);
		int count = json_doc_array_size(d, path);
		long best = LONG_MAX;
		bool adequate = false;
		/* Image arrays are tiny; a hostile response must not create a long walk. */
		for (int i = 0; i < count && i < 32; i++) {
			char relative[64], candidate[256] = "";
			snprintf(relative, sizeof relative, "%s[%d].url", array, i);
			field_path(path, sizeof path, base, relative);
			if (!read_string(d, path, candidate, sizeof candidate, true))
				continue;
			long width = 0;
			snprintf(relative, sizeof relative, "%s[%d].width", array, i);
			field_path(path, sizeof path, base, relative);
			json_doc_int(d, path, &width);
			bool fits = width >= 52;
			if (!parsed.art[0] || (fits && (!adequate || width < best)) ||
			    (!adequate && !fits && width > best)) {
				memcpy(parsed.art, candidate, sizeof parsed.art);
				best = width;
				adequate = fits;
			}
		}
		if (parsed.art[0])
			parsed.art_source = META_API;
	}
	playlist_meta_merge(meta, &parsed);
}

void playlist_meta_parse(playlist_meta *meta, const char *body, unsigned len,
                         int phase)
{
	if (!body || !len || len > 65536)
		return;
	json_doc *d = json_doc_parse(body, len, NULL);
	if (!d)
		return;
	playlist_meta_parse_doc(meta, d, "", phase);
	json_doc_free(d);
}

bool playlist_meta_begin(playlist_meta_job *job, const char *uri,
                         const playlist_meta *seed)
{
	memset(job, 0, sizeof *job);
	const char *prefix = "spotify:playlist:";
	if (!uri || strncmp(uri, prefix, strlen(prefix)) != 0)
		return false;
	const char *id = uri + strlen(prefix);
	size_t n = strlen(id);
	if (n < 8 || n >= sizeof job->id)
		return false;
	for (size_t i = 0; i < n; i++)
		if (!((id[i] >= 'a' && id[i] <= 'z') ||
		      (id[i] >= 'A' && id[i] <= 'Z') ||
		      (id[i] >= '0' && id[i] <= '9')))
			return false;
	snprintf(job->uri, sizeof job->uri, "%s", uri);
	memcpy(job->id, id, n + 1);
	if (seed)
		job->meta = *seed;
	return true;
}

void playlist_meta_reset_backoff(void)
{
	s_api_retry = s_embed_retry = 0;
}

bool playlist_meta_step(playlist_meta_job *job)
{
	if (job->phase >= 3)
		return true;
	/* A fallback field should still be upgraded by a fresh Web API request. */
	if (job->phase == 1 && job->meta.art_source == META_API && job->meta.art[0])
		job->phase++;
	if (job->phase == 2 && playlist_meta_complete(&job->meta)) {
		job->phase = 3;
		return true;
	}
	const int phase = job->phase++;
	const time_t now = time(NULL);
	time_t *backoff = phase == 2 ? &s_embed_retry : &s_api_retry;
	if (*backoff > now) {
		job->retry_at = *backoff;
		return job->phase >= 3;
	}
	char path[256], err[128];
	const char *host = phase == 2 ? "open.spotify.com" : "api.spotify.com";
	const char *token = NULL;
	if (phase == 2) {
		snprintf(path, sizeof path,
		         "/oembed?url=https%%3A%%2F%%2Fopen.spotify.com%%2Fplaylist%%2F%s&format=json",
		         job->id);
	} else {
		token = auth_token(err, sizeof err);
		if (!token) {
			job->retry_at = now + 60;
			job->phase = 3; /* Public metadata is not an authentication repair. */
			return true;
		}
		snprintf(path, sizeof path,
		         phase == 0 ? "/v1/playlists/%s?fields=name,images,owner(display_name)"
		                    : "/v1/playlists/%s/images", job->id);
	}
	http_response r;
	if (!http_request(host, "GET", path, token, NULL, NULL, &r, err, sizeof err)) {
		*backoff = now + 60;
		job->retry_at = *backoff;
		tl_log("playlist display %s phase %d: %s", job->id, phase, err);
	} else {
		if (r.status == 200) {
			playlist_meta parsed = {0};
			playlist_meta_parse(&parsed, r.body, (unsigned)r.body_len, phase);
			if (parsed.name_source != META_NONE &&
			    parsed.name_source >= job->meta.name_source)
				job->fetched_name = true;
			if (parsed.art_source != META_NONE &&
			    parsed.art_source >= job->meta.art_source)
				job->fetched_art = true;
			playlist_meta_merge(&job->meta, &parsed);
		}
		else {
			tl_log("playlist display %s phase %d: http %d", job->id, phase, r.status);
			if (r.status == 429 || r.status >= 500 || r.status == 401) {
				long wait = r.retry_after > 0 ? r.retry_after : 60;
				if (wait > 7 * 24 * 3600)
					wait = 7 * 24 * 3600;
				*backoff = now + wait;
				job->retry_at = *backoff;
			}
		}
		http_free(&r);
	}
	return job->phase >= 3;
}
