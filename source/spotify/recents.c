#include "recents.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "../net/http.h"
#include "../testlog.h"
#include "auth.h"
#include "json.h"
#include "namecache.h"

#define API_HOST "api.spotify.com"
/* U+00B7, verified against the shared 3DS system font by ui_font_probe(). */
#define SUB_SEP " \xC2\xB7 "

/* The most Spotify will return in one page.
 *
 * Asking for far more items than the shelf shows is the point: entries collapse
 * by collection, and a user listening through one album yields a single tile
 * from dozens of tracks. Measured on this account, 50 raw items deduped to 4
 * distinct collections, so a smaller limit would leave the Library screen
 * nearly empty.
 *
 * Note this endpoint ignores `fields=` - it returns items of `{}` rather than a
 * trimmed object - so the full ~147KB body has to come down and be parsed. That
 * is what sets the json token pool ceiling. */
#define FETCH_LIMIT 50

/* Copy the last path-ish segment of a spotify uri: the id after the final
 * colon. Returns false when the uri is not of the expected shape. */
static bool uri_id(const char *uri, char *out, int outlen)
{
	const char *colon = strrchr(uri, ':');
	if (!colon || !colon[1])
		return false;
	snprintf(out, outlen, "%s", colon + 1);
	return true;
}

/* GET /v1/playlists/{id}?fields=name,images,owner(display_name).
 *
 * recently-played gives a context uri but never the playlist's name or artwork,
 * so this is the only way to label one correctly. The art matters as much as
 * the name: the enclosing item's images are the *album cover of the track that
 * happened to be playing*, so using those showed a playlist under an unrelated
 * cover. Results go through namecache, so this runs once per playlist rather
 * than once per launch. */
bool playlist_metadata(const char *uri, char *name, int namelen, char *owner,
                       int ownerlen, char *art, int artlen)
{
	name[0]  = '\0';
	owner[0] = '\0';
	art[0]   = '\0';

	/* A hit is only useful if it has the artwork too. Entries written before
	 * the art column existed read back with an empty url, and returning them
	 * as-is stuck those playlists on the wrong cover permanently: the cache
	 * answered, so the request that would have filled it never ran. Treating
	 * that as a miss lets an old cache heal itself on the next launch.
	 *
	 * A playlist genuinely without art re-requests once per launch as a
	 * result. That is a handful of requests at most, and only for playlists
	 * that have no image to find. */
	if (namecache_get(uri, name, namelen, owner, ownerlen, art, artlen) &&
	    art[0])
		return true;

	/* Fall through to the fetch, discarding the partial hit. */
	name[0]  = '\0';
	owner[0] = '\0';
	art[0]   = '\0';

	char id[64];
	if (!uri_id(uri, id, sizeof id))
		return false;

	/* auth_token writes into err on failure, so give it somewhere to write:
	 * snprintf into NULL is undefined, not a no-op. */
	char        aerr[128];
	const char *token = auth_token(aerr, sizeof aerr);
	if (!token)
		return false;

	char path[192];
	snprintf(path, sizeof path,
	         "/v1/playlists/%s?fields=name,images,owner(display_name)", id);

	char         err[128];
	http_response r;
	if (!http_request(API_HOST, "GET", path, token, NULL, NULL, &r, err,
	                  sizeof err))
		return false;

	if (r.status != 200 || !r.body || r.body_len == 0) {
		/* A playlist can be deleted or made private after being played, which
		 * shows up as 404/403. Not an error worth failing the whole list over. */
		tl_log("playlist meta %s: http %d", id, r.status);
		http_free(&r);
		return false;
	}

	const bool ok = json_get_str(r.body, r.body_len, "name", name, (size_t)namelen);
	json_get_str(r.body, r.body_len, "owner.display_name", owner,
	             (size_t)ownerlen);

	/* Mosaics come in 640/300/60; a plain uploaded cover may be a single entry.
	 * Prefer the smallest, which at 60px is already larger than the 52px tile. */
	if (!json_get_str(r.body, r.body_len, "images[2].url", art, (size_t)artlen))
		json_get_str(r.body, r.body_len, "images[0].url", art, (size_t)artlen);

	http_free(&r);

	if (ok && name[0])
		namecache_put(uri, name, owner, art);

	return ok && name[0];
}

player_result recents_fetch(recent_list *out, char *err, int errlen)
{
	memset(out, 0, sizeof *out);

	const char *token = auth_token(err, errlen);
	if (!token)
		return PLAYER_AUTH_FAILED;

	char path[80];
	snprintf(path, sizeof path, "/v1/me/player/recently-played?limit=%d",
	         FETCH_LIMIT);

	http_response r;
	if (!http_request(API_HOST, "GET", path, token, NULL, NULL, &r, err, errlen))
		return PLAYER_ERROR;

	if (r.status == 403) {
		/* Missing scope. Worth naming precisely: the symptom otherwise looks
		 * identical to an empty listening history. */
		snprintf(err, errlen,
		         "403 - token lacks user-read-recently-played; re-run "
		         "bootstrap_auth.py");
		http_free(&r);
		return PLAYER_FORBIDDEN;
	}
	if (r.status != 200 || !r.body || r.body_len == 0) {
		snprintf(err, errlen, "recents http %d", r.status);
		http_free(&r);
		return PLAYER_ERROR;
	}

	const u64 t0 = osGetTime();

	int       needed = 0;
	json_doc *d      = json_doc_parse(r.body, r.body_len, &needed);
	if (!d) {
		/* Distinguish "too big for the token pool" from malformed JSON: the
		 * first is a tuning problem, the second a protocol change, and they
		 * would otherwise look the same from a transcript. */
		snprintf(err, errlen, "recents parse failed (tokens %d, %u bytes)",
		         needed, (unsigned)r.body_len);
		http_free(&r);
		return PLAYER_ERROR;
	}

	tl_timing("recents parse: %u bytes %d tokens in %llums",
	          (unsigned)r.body_len, json_doc_tokens(d),
	          (unsigned long long)(osGetTime() - t0));

	for (int i = 0; i < FETCH_LIMIT && out->count < RECENTS_MAX; i++) {
		char p[96];
		char album[128] = "", artist[128] = "", album_uri[128] = "",
		     art[256] = "";

		snprintf(p, sizeof p, "items[%d].track.album.name", i);
		if (!json_doc_str(d, p, album, sizeof album))
			break; /* ran out of items */

		snprintf(p, sizeof p, "items[%d].track.album.uri", i);
		json_doc_str(d, p, album_uri, sizeof album_uri);

		/* Prefer the context the track was played from (a playlist, say) so
		 * tapping resumes what the user was actually listening to; fall back to
		 * the album when Spotify reports no context. */
		char ctx[128] = "";
		snprintf(p, sizeof p, "items[%d].context.uri", i);
		json_doc_str(d, p, ctx, sizeof ctx);

		const bool  is_playlist = strncmp(ctx, "spotify:playlist:", 17) == 0;
		const char *play_uri    = is_playlist ? ctx : album_uri;

		if (!play_uri[0])
			continue; /* nothing to play; skip rather than send a bad body */

		snprintf(p, sizeof p, "items[%d].track.artists[0].name", i);
		json_doc_str(d, p, artist, sizeof artist);

		/* images[0] is the 640px cover; the shelf wants something far smaller,
		 * and Spotify offers a 64px variant as the last entry. */
		snprintf(p, sizeof p, "items[%d].track.album.images[2].url", i);
		if (!json_doc_str(d, p, art, sizeof art)) {
			snprintf(p, sizeof p, "items[%d].track.album.images[0].url", i);
			json_doc_str(d, p, art, sizeof art);
		}

		/* Collapse repeats of the same collection anywhere in the page, not
		 * just consecutive ones: an album returned to later in the session is
		 * still the same tile. */
		bool dup = false;
		for (int k = 0; k < out->count; k++) {
			if (strcmp(out->items[k].context_uri, play_uri) == 0) {
				dup = true;
				break;
			}
		}
		if (dup)
			continue;

		collection_item *it = &out->items[out->count++];

		if (is_playlist) {
			char pname[128] = "", powner[128] = "", part[256] = "";

			if (playlist_metadata(play_uri, pname, sizeof pname, powner,
			                      sizeof powner, part, sizeof part)) {
				snprintf(it->name, sizeof it->name, "%s", pname);
				snprintf(it->subtitle, sizeof it->subtitle, "Playlist" SUB_SEP "%s",
				         powner[0] ? powner : artist);

				/* The playlist's own cover, not `art` - that is the album of
				 * whichever track happened to be playing when the context was
				 * recorded, which is a different picture entirely. */
				if (part[0])
					snprintf(art, sizeof art, "%s", part);
			} else {
				/* Naming failed (deleted, private, or offline). Label it by the
				 * track it was reached through rather than claiming it is an
				 * album, which would be plainly wrong about what tapping does.
				 * The track's album art stays as a stand-in - a wrong-but-real
				 * cover beats an empty tile when we know nothing else. */
				snprintf(p, sizeof p, "items[%d].track.name", i);
				char track[128] = "";
				json_doc_str(d, p, track, sizeof track);
				snprintf(it->name, sizeof it->name, "%s",
				         track[0] ? track : album);
				snprintf(it->subtitle, sizeof it->subtitle, "Playlist" SUB_SEP "%s",
				         artist);
			}
			it->kind = COLLECTION_PLAYLIST;
		} else {
			snprintf(it->name, sizeof it->name, "%s", album);
			snprintf(it->subtitle, sizeof it->subtitle, "Album" SUB_SEP "%s", artist);
			it->kind = COLLECTION_ALBUM;
		}

		snprintf(it->art_url, sizeof it->art_url, "%s", art);
		snprintf(it->context_uri, sizeof it->context_uri, "%s", play_uri);
	}

	json_doc_free(d);
	http_free(&r);

	tl_log("recents: %d distinct of %d fetched", out->count, FETCH_LIMIT);
	return out->count > 0 ? PLAYER_OK : PLAYER_NOTHING_PLAYING;
}

player_result playlists_fetch(playlist_list *out, char *err, int errlen)
{
	memset(out, 0, sizeof *out);

	const char *token = auth_token(err, errlen);
	if (!token)
		return PLAYER_AUTH_FAILED;

	for (int offset = 0; offset < PLAYLISTS_MAX; offset += FETCH_LIMIT) {
		char path[256];
		snprintf(path, sizeof path,
		         "/v1/me/playlists?limit=%d&offset=%d&fields=total,items(name,uri,"
		         "images,owner(display_name),items(total))",
		         FETCH_LIMIT, offset);

		http_response r;
		if (!http_request(API_HOST, "GET", path, token, NULL, NULL, &r, err,
		                  errlen))
			return out->count ? PLAYER_OK : PLAYER_ERROR;
		if (r.status == 403) {
			snprintf(err, errlen,
			         "403 - token lacks playlist-read-private; re-run "
			         "bootstrap_auth.py");
			http_free(&r);
			return out->count ? PLAYER_OK : PLAYER_FORBIDDEN;
		}
		if (r.status != 200 || !r.body || !r.body_len) {
			snprintf(err, errlen, "playlists http %d", r.status);
			http_free(&r);
			return out->count ? PLAYER_OK : PLAYER_ERROR;
		}

		const u64 t0 = osGetTime();
		int needed = 0;
		json_doc *d = json_doc_parse(r.body, r.body_len, &needed);
		if (!d) {
			snprintf(err, errlen, "playlists parse failed (tokens %d, %u bytes)",
			         needed, (unsigned)r.body_len);
			http_free(&r);
			return out->count ? PLAYER_OK : PLAYER_ERROR;
		}

		long total = 0;
		if (json_doc_int(d, "total", &total))
			out->total = (int)total;
		int page_count = json_doc_array_size(d, "items");
		if (page_count < 0)
			page_count = 0;

		for (int i = 0; i < page_count && out->count < PLAYLISTS_MAX; i++) {
			char p[96];
			char name[128] = "", uri[128] = "", owner[128] = "", art[256] = "";
			snprintf(p, sizeof p, "items[%d].name", i);
			if (!json_doc_str(d, p, name, sizeof name))
				continue;
			snprintf(p, sizeof p, "items[%d].uri", i);
			if (!json_doc_str(d, p, uri, sizeof uri) || !uri[0])
				continue;
			snprintf(p, sizeof p, "items[%d].owner.display_name", i);
			if (!json_doc_is_null(d, p))
				json_doc_str(d, p, owner, sizeof owner);
			long item_total = 0;
			snprintf(p, sizeof p, "items[%d].items.total", i);
			json_doc_int(d, p, &item_total);
			snprintf(p, sizeof p, "items[%d].images[2].url", i);
			if (!json_doc_str(d, p, art, sizeof art)) {
				snprintf(p, sizeof p, "items[%d].images[0].url", i);
				json_doc_str(d, p, art, sizeof art);
			}

			collection_item *it = &out->items[out->count++];
			snprintf(it->name, sizeof it->name, "%s", name);
			snprintf(it->subtitle, sizeof it->subtitle,
			         owner[0] ? "Playlist" SUB_SEP "%s" : "Playlist", owner);
			snprintf(it->art_url, sizeof it->art_url, "%s", art);
			snprintf(it->context_uri, sizeof it->context_uri, "%s", uri);
			it->item_total = (int)item_total;
			it->kind = COLLECTION_PLAYLIST;
			namecache_put(uri, name, owner, art);
		}

		tl_timing("playlists page offset=%d bytes=%u tokens=%d in %llums", offset,
		          (unsigned)r.body_len, json_doc_tokens(d),
		          (unsigned long long)(osGetTime() - t0));
		json_doc_free(d);
		http_free(&r);
		/* Spotify may omit an inaccessible/null playlist from the array while
		 * still counting its slot in total. Advance by the requested offset, not
		 * by parsed rows, or that omission hides every later page. */
		if (offset + FETCH_LIMIT >= out->total)
			break;
	}

	tl_log("playlists: %d of %d total", out->count, out->total);
	return out->count > 0 ? PLAYER_OK : PLAYER_NOTHING_PLAYING;
}

player_result albums_fetch(album_list *out, char *err, int errlen)
{
	memset(out, 0, sizeof *out);

	const char *token = auth_token(err, errlen);
	if (!token)
		return PLAYER_AUTH_FAILED;

	for (int offset = 0; offset < ALBUMS_MAX; offset += FETCH_LIMIT) {
		char path[256];
		snprintf(path, sizeof path,
		         "/v1/me/albums?limit=%d&offset=%d&fields=total,items(album(name,"
		         "uri,total_tracks,artists(name),images))",
		         FETCH_LIMIT, offset);
		http_response r;
		if (!http_request(API_HOST, "GET", path, token, NULL, NULL, &r, err,
		                  errlen))
			return out->count ? PLAYER_OK : PLAYER_ERROR;
		if (r.status == 403) {
			snprintf(err, errlen,
			         "403 - token lacks user-library-read; re-run bootstrap_auth.py");
			http_free(&r);
			return out->count ? PLAYER_OK : PLAYER_FORBIDDEN;
		}
		if (r.status != 200 || !r.body || !r.body_len) {
			snprintf(err, errlen, "albums http %d", r.status);
			http_free(&r);
			return out->count ? PLAYER_OK : PLAYER_ERROR;
		}

		const u64 t0 = osGetTime();
		int needed = 0;
		json_doc *d = json_doc_parse(r.body, r.body_len, &needed);
		if (!d) {
			snprintf(err, errlen, "albums parse failed (tokens %d, %u bytes)",
			         needed, (unsigned)r.body_len);
			http_free(&r);
			return out->count ? PLAYER_OK : PLAYER_ERROR;
		}
		long total = 0;
		if (json_doc_int(d, "total", &total))
			out->total = (int)total;
		int page_count = json_doc_array_size(d, "items");
		if (page_count < 0)
			page_count = 0;

		for (int i = 0; i < page_count && out->count < ALBUMS_MAX; i++) {
			char p[96];
			char name[128] = "", uri[128] = "", artist[128] = "", art[256] = "";
			snprintf(p, sizeof p, "items[%d].album.name", i);
			if (!json_doc_str(d, p, name, sizeof name))
				continue;
			snprintf(p, sizeof p, "items[%d].album.uri", i);
			if (!json_doc_str(d, p, uri, sizeof uri) || !uri[0])
				continue;
			snprintf(p, sizeof p, "items[%d].album.artists[0].name", i);
			json_doc_str(d, p, artist, sizeof artist);
			long item_total = 0;
			snprintf(p, sizeof p, "items[%d].album.total_tracks", i);
			json_doc_int(d, p, &item_total);
			snprintf(p, sizeof p, "items[%d].album.images[2].url", i);
			if (!json_doc_str(d, p, art, sizeof art)) {
				snprintf(p, sizeof p, "items[%d].album.images[0].url", i);
				json_doc_str(d, p, art, sizeof art);
			}
			collection_item *it = &out->items[out->count++];
			snprintf(it->name, sizeof it->name, "%s", name);
			snprintf(it->subtitle, sizeof it->subtitle,
			         artist[0] ? "Album" SUB_SEP "%s" : "Album", artist);
			snprintf(it->art_url, sizeof it->art_url, "%s", art);
			snprintf(it->context_uri, sizeof it->context_uri, "%s", uri);
			it->item_total = (int)item_total;
			it->kind = COLLECTION_ALBUM;
		}

		tl_timing("albums page offset=%d bytes=%u tokens=%d in %llums", offset,
		          (unsigned)r.body_len, json_doc_tokens(d),
		          (unsigned long long)(osGetTime() - t0));
		json_doc_free(d);
		http_free(&r);
		if (offset + FETCH_LIMIT >= out->total)
			break;
	}

	tl_log("albums: %d of %d total", out->count, out->total);
	return out->count > 0 ? PLAYER_OK : PLAYER_NOTHING_PLAYING;
}
