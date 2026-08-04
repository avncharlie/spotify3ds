#include "recents.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "../net/http.h"
#include "../testlog.h"
#include "auth.h"
#include "json.h"

#define API_HOST "api.spotify.com"

/* Ask for more items than the shelf shows, because consecutive tracks from one
 * album collapse to a single entry: listening through one record would
 * otherwise yield a shelf of the same cover four times. 10 raw items reliably
 * yields enough distinct albums without making the response large. */
#define FETCH_LIMIT 10

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
		char album[128] = "", artist[128] = "", uri[128] = "", art[256] = "";

		snprintf(p, sizeof p, "items[%d].track.album.name", i);
		if (!json_doc_str(d, p, album, sizeof album))
			break; /* ran out of items */

		snprintf(p, sizeof p, "items[%d].track.album.uri", i);
		json_doc_str(d, p, uri, sizeof uri);

		/* Prefer the context the track was played from (a playlist, say) so
		 * tapping resumes what the user was actually listening to; fall back to
		 * the album when Spotify reports no context. */
		char ctx[128] = "";
		snprintf(p, sizeof p, "items[%d].context.uri", i);
		json_doc_str(d, p, ctx, sizeof ctx);

		const char *play_uri = ctx[0] ? ctx : uri;
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

		/* Collapse consecutive plays from the same album. */
		bool dup = false;
		for (int k = 0; k < out->count; k++) {
			if (strcmp(out->items[k].context_uri, play_uri) == 0) {
				dup = true;
				break;
			}
		}
		if (dup)
			continue;

		recent_item *it = &out->items[out->count++];

		/* Label by what tapping will actually play. Spotify only gives the
		 * context's *uri* here, not its name, so a playlist has to be
		 * described by the track it was reached through - saying
		 * "Album - Artist" over a playlist uri would be plainly wrong, and the
		 * album name shown would not be what starts playing. */
		const bool is_playlist = strncmp(play_uri, "spotify:playlist:", 17) == 0;

		if (is_playlist) {
			char track[128] = "";
			snprintf(p, sizeof p, "items[%d].track.name", i);
			json_doc_str(d, p, track, sizeof track);
			snprintf(it->name, sizeof it->name, "%s",
			         track[0] ? track : album);
			snprintf(it->subtitle, sizeof it->subtitle, "Playlist - %s",
			         artist);
		} else {
			snprintf(it->name, sizeof it->name, "%s", album);
			snprintf(it->subtitle, sizeof it->subtitle, "Album - %s", artist);
		}

		snprintf(it->art_url, sizeof it->art_url, "%s", art);
		snprintf(it->context_uri, sizeof it->context_uri, "%s", play_uri);
	}

	json_doc_free(d);
	http_free(&r);

	tl_log("recents: %d distinct of %d fetched", out->count, FETCH_LIMIT);
	return out->count > 0 ? PLAYER_OK : PLAYER_NOTHING_PLAYING;
}
