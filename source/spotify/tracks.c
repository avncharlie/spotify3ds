#include "tracks.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "auth.h"
#include "json.h"
#include "../net/http.h"
#include "../testlog.h"

#define API_HOST "api.spotify.com"

static bool uri_id(const char *uri, collection_kind kind, char *out, int outlen)
{
	const char *prefix = kind == COLLECTION_PLAYLIST ? "spotify:playlist:"
	                                                  : "spotify:album:";
	const size_t n = strlen(prefix);
	if (!uri || strncmp(uri, prefix, n) != 0)
		return false;
	const char *id = uri + n;
	int len = 0;
	while (id[len]) {
		const char c = id[len];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9')))
			return false;
		len++;
	}
	if (len < 8 || len >= outlen)
		return false;
	memcpy(out, id, (size_t)len + 1);
	return true;
}

static void join_artists(const json_doc *d, const char *base, char *out,
	                     int outlen)
{
	out[0] = '\0';
	for (int i = 0; i < 4; i++) {
		char path[192], name[96];
		snprintf(path, sizeof path, "%s.artists[%d].name", base, i);
		if (!json_doc_str(d, path, name, sizeof name))
			break;
		const size_t used = strlen(out);
		snprintf(out + used, (size_t)outlen - used, "%s%s",
		         used ? ", " : "", name);
	}
}

static player_result fetch_network(const collection_item *collection, int offset,
	                               track_page *out, char *err, int errlen)
{
	char id[64];
	if (!uri_id(collection->context_uri, collection->kind, id, sizeof id)) {
		snprintf(err, errlen, "invalid collection uri");
		return PLAYER_ERROR;
	}

	const char *token = auth_token(err, errlen);
	if (!token)
		return PLAYER_AUTH_FAILED;

	char path[640];
	if (collection->kind == COLLECTION_ALBUM) {
		snprintf(path, sizeof path,
		         "/v1/albums/%s/tracks?market=from_token&limit=%d&offset=%d", id,
		         TRACK_PAGE_MAX, offset);
	} else {
		snprintf(path, sizeof path,
		         "/v1/playlists/%s/items?market=from_token&limit=%d&offset=%d&"
		         "additional_types=track,episode&fields=total,limit,offset,next,"
		         "items(is_local,item(type,uri,name,duration_ms,is_playable,"
		         "explicit,artists(name),album(name,images)))",
		         id, TRACK_PAGE_MAX, offset);
	}

	http_response r;
	const u64 t0 = osGetTime();
	if (!http_request(API_HOST, "GET", path, token, NULL, NULL, &r, err, errlen))
		return PLAYER_ERROR;

	if (r.status == 403) {
		snprintf(err, errlen, "Spotify only exposes tracks for owned or collaborative playlists");
		http_free(&r);
		return PLAYER_FORBIDDEN;
	}
	if (r.status != 200 || !r.body || !r.body_len) {
		snprintf(err, errlen, "tracks http %d", r.status);
		http_free(&r);
		return PLAYER_ERROR;
	}

	int needed = 0;
	json_doc *d = json_doc_parse(r.body, r.body_len, &needed);
	if (!d) {
		snprintf(err, errlen, "tracks parse failed (%u bytes, tokens %d)",
		         (unsigned)r.body_len, needed);
		http_free(&r);
		return PLAYER_ERROR;
	}

	memset(out, 0, sizeof *out);
	out->collection = *collection;
	out->offset = offset;
	long value = 0;
	if (json_doc_int(d, "offset", &value))
		out->offset = (int)value;
	if (json_doc_int(d, "total", &value))
		out->total = (int)value;
	else
		out->total = collection->item_total;

	int count = json_doc_array_size(d, "items");
	if (count < 0)
		count = 0;
	if (count > TRACK_PAGE_MAX)
		count = TRACK_PAGE_MAX;
	out->count = count;

	for (int i = 0; i < count; i++) {
		track_item *it = &out->items[i];
		it->source_index = out->offset + i;
		it->kind = TRACK_ITEM_UNAVAILABLE;
		snprintf(it->art_url, sizeof it->art_url, "%s", collection->art_url);

		char base[64];
		if (collection->kind == COLLECTION_ALBUM)
			snprintf(base, sizeof base, "items[%d]", i);
		else
			snprintf(base, sizeof base, "items[%d].item", i);

		if (json_doc_is_null(d, base)) {
			snprintf(it->name, sizeof it->name, "Unavailable item");
			it->kind = TRACK_ITEM_UNAVAILABLE;
			continue;
		}

		char field[192], type[32] = "";
		snprintf(field, sizeof field, "%s.type", base);
		json_doc_str(d, field, type, sizeof type);
		if (strcmp(type, "track") == 0)
			it->kind = TRACK_ITEM_TRACK;
		else if (strcmp(type, "episode") == 0)
			it->kind = TRACK_ITEM_EPISODE;

		snprintf(field, sizeof field, "%s.name", base);
		if (!json_doc_str(d, field, it->name, sizeof it->name))
			snprintf(it->name, sizeof it->name, "Unavailable item");
		snprintf(field, sizeof field, "%s.uri", base);
		json_doc_str(d, field, it->uri, sizeof it->uri);
		snprintf(field, sizeof field, "%s.duration_ms", base);
		if (json_doc_int(d, field, &value))
			it->duration_ms = value;
		snprintf(field, sizeof field, "%s.is_playable", base);
		bool flag = false;
		const bool have_playable = json_doc_bool(d, field, &flag);
		it->playable = have_playable ? flag : it->kind == TRACK_ITEM_TRACK;
		snprintf(field, sizeof field, "%s.explicit", base);
		if (json_doc_bool(d, field, &flag))
			it->explicit_content = flag;
		if (collection->kind == COLLECTION_PLAYLIST) {
			snprintf(field, sizeof field, "items[%d].is_local", i);
			if (json_doc_bool(d, field, &flag))
				it->is_local = flag;
			snprintf(field, sizeof field, "%s.album.images[2].url", base);
			if (!json_doc_str(d, field, it->art_url, sizeof it->art_url)) {
				snprintf(field, sizeof field, "%s.album.images[0].url", base);
				json_doc_str(d, field, it->art_url, sizeof it->art_url);
			}
			snprintf(field, sizeof field, "%s.album.name", base);
			json_doc_str(d, field, it->album, sizeof it->album);
		} else {
			snprintf(it->album, sizeof it->album, "%s", collection->name);
		}
		join_artists(d, base, it->artist, sizeof it->artist);

		if (!it->uri[0] || it->is_local || it->kind != TRACK_ITEM_TRACK)
			it->playable = false;
	}

	json_doc_free(d);
	tl_timing("tracks %s offset=%d count=%d total=%d bytes=%u tokens=%d took=%llums",
	          collection->kind == COLLECTION_PLAYLIST ? "playlist" : "album",
	          out->offset, out->count, out->total, (unsigned)r.body_len, needed,
	          (unsigned long long)(osGetTime() - t0));
	http_free(&r);
	return PLAYER_OK;
}

player_result tracks_fetch_page(const collection_item *collection, int offset,
	                            track_page *out, char *err, int errlen)
{
	if (!collection || !collection->context_uri[0]) {
		snprintf(err, errlen, "no collection");
		return PLAYER_ERROR;
	}
	if (offset < 0)
		offset = 0;
	offset = (offset / TRACK_PAGE_MAX) * TRACK_PAGE_MAX;
	return fetch_network(collection, offset, out, err, errlen);
}
