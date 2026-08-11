#include "player.h"

#include <stdio.h>
#include <string.h>

#include "../net/http.h"
#include "../testlog.h"
#include "auth.h"
#include "json.h"

#define API_HOST "api.spotify.com"

const char *player_result_str(player_result r)
{
	switch (r) {
		case PLAYER_OK:              return "ok";
		case PLAYER_NOTHING_PLAYING: return "nothing playing";
		case PLAYER_NO_DEVICE:       return "no active device";
		case PLAYER_FORBIDDEN:       return "forbidden (Premium required?)";
		case PLAYER_AUTH_FAILED:     return "auth failed";
		default:                     return "error";
	}
}

static player_result status_to_result(int status)
{
	switch (status) {
		case 200:
		case 202:
		case 204: return PLAYER_OK;
		case 401: return PLAYER_AUTH_FAILED;
		case 403: return PLAYER_FORBIDDEN;
		case 404: return PLAYER_NO_DEVICE;
		default:  return PLAYER_ERROR;
	}
}

/* Issue an authenticated request, transparently refreshing once on 401.
 * Caller owns *resp on PLAYER_OK. */
static player_result api_call(const char *method, const char *path,
                              const char *ctype, const char *body,
                              http_response *resp, char *err, int errlen)
{
	const char *token = auth_token(err, errlen);
	if (!token)
		return PLAYER_AUTH_FAILED;

	if (!http_request(API_HOST, method, path, token, ctype, body, resp, err,
	                  errlen))
		return PLAYER_ERROR;

	/* A token can be revoked or expire early; one forced refresh distinguishes
	 * that from genuinely broken credentials. */
	if (resp->status == 401) {
		http_free(resp);
		tl_log("401, forcing token refresh");
		if (!auth_refresh(err, errlen))
			return PLAYER_AUTH_FAILED;

		token = auth_token(err, errlen);
		if (!token)
			return PLAYER_AUTH_FAILED;

		if (!http_request(API_HOST, method, path, token, ctype, body, resp, err,
		                  errlen))
			return PLAYER_ERROR;
	}

	return status_to_result(resp->status);
}

player_result player_poll(player_state *out, char *err, int errlen)
{
	memset(out, 0, sizeof *out);

	/* /me/player rather than /me/player/currently-playing: it is a superset
	 * that also carries shuffle_state, which the UI needs. */
	http_response  r;
	player_result  pr = api_call("GET", "/v1/me/player", NULL, NULL, &r, err,
	                             errlen);
	if (pr != PLAYER_OK) {
		if (pr == PLAYER_ERROR)
			snprintf(err, errlen, "poll failed");
		return pr;
	}

	/* 204 means nothing is playing. This is a normal state, not an error:
	 * treating it as a failure makes the app look broken whenever the phone
	 * is idle. */
	if (r.status == 204 || !r.body || r.body_len == 0) {
		http_free(&r);
		return PLAYER_NOTHING_PLAYING;
	}

	const char *j = r.body;
	size_t      n = r.body_len;

	json_get_str(j, n, "item.name", out->track, sizeof out->track);
	json_get_str(j, n, "item.artists[0].name", out->artist, sizeof out->artist);
	json_get_str(j, n, "item.album.name", out->album, sizeof out->album);
	json_get_str(j, n, "item.album.images[0].url", out->art_url,
	             sizeof out->art_url);
	json_get_str(j, n, "item.uri", out->track_uri, sizeof out->track_uri);
	json_get_str(j, n, "item.album.uri", out->album_uri,
	             sizeof out->album_uri);
	json_get_str(j, n, "context.uri", out->context_uri,
	             sizeof out->context_uri);
	json_get_int(j, n, "progress_ms", &out->progress_ms);
	json_get_int(j, n, "item.duration_ms", &out->duration_ms);
	json_get_bool(j, n, "is_playing", &out->is_playing);
	json_get_bool(j, n, "shuffle_state", &out->shuffle);

	/* Which device the audio is actually coming out of. Already in this
	 * response, so the UI's device line costs no extra request. */
	json_get_str(j, n, "device.id", out->device_id, sizeof out->device_id);
	json_get_str(j, n, "device.name", out->device_name, sizeof out->device_name);
	json_get_str(j, n, "device.type", out->device_type, sizeof out->device_type);
	out->volume_known = json_get_int(j, n, "device.volume_percent",
	                                 &out->volume_percent);
	json_get_bool(j, n, "device.supports_volume", &out->supports_volume);

	char rep[16] = "";
	if (json_get_str(j, n, "repeat_state", rep, sizeof rep)) {
		if (strcmp(rep, "track") == 0)
			out->repeat = REPEAT_TRACK;
		else if (strcmp(rep, "context") == 0)
			out->repeat = REPEAT_CONTEXT;
		else
			out->repeat = REPEAT_OFF;
	}

	http_free(&r);

	/* Ads and podcast episodes come back without a track name; report them as
	 * "nothing playing" rather than rendering a blank screen. */
	if (!out->track[0])
		return PLAYER_NOTHING_PLAYING;

	return PLAYER_OK;
}

/* The control endpoints take no body, but Spotify rejects a PUT that omits
 * Content-Length, which http_request always sends. */
static player_result simple_cmd(const char *method, const char *path,
                                char *err, int errlen)
{
	http_response r;
	player_result pr = api_call(method, path, NULL, NULL, &r, err, errlen);
	if (pr == PLAYER_OK || r.body)
		http_free(&r);
	return pr;
}

player_result player_play(char *err, int errlen)
{
	return simple_cmd("PUT", "/v1/me/player/play", err, errlen);
}

player_result player_pause(char *err, int errlen)
{
	return simple_cmd("PUT", "/v1/me/player/pause", err, errlen);
}

player_result player_next(char *err, int errlen)
{
	return simple_cmd("POST", "/v1/me/player/next", err, errlen);
}

player_result player_prev(char *err, int errlen)
{
	return simple_cmd("POST", "/v1/me/player/previous", err, errlen);
}

player_result player_queue_item(const char *item_uri, char *err, int errlen)
{
	static const char prefix[] = "spotify:track:";
	if (!item_uri || strncmp(item_uri, prefix, sizeof prefix - 1) != 0) {
		snprintf(err, errlen, "invalid queue item uri");
		return PLAYER_ERROR;
	}
	const char *id = item_uri + sizeof prefix - 1;
	if (!id[0]) {
		snprintf(err, errlen, "invalid queue item uri");
		return PLAYER_ERROR;
	}
	for (const char *p = id; *p; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9'))) {
			snprintf(err, errlen, "invalid queue item uri");
			return PLAYER_ERROR;
		}
	}

	char path[192];
	snprintf(path, sizeof path,
	         "/v1/me/player/queue?uri=spotify%%3Atrack%%3A%s", id);
	return simple_cmd("POST", path, err, errlen);
}

player_result player_seek(long position_ms, char *err, int errlen)
{
	if (position_ms < 0)
		position_ms = 0;

	char path[96];
	snprintf(path, sizeof path, "/v1/me/player/seek?position_ms=%ld",
	         position_ms);
	return simple_cmd("PUT", path, err, errlen);
}

player_result player_set_volume(int volume_percent, const char *device_id,
                                char *err, int errlen)
{
	if (volume_percent < 0 || volume_percent > 100) {
		snprintf(err, errlen, "invalid volume %d", volume_percent);
		return PLAYER_ERROR;
	}

	char path[512];
	if (device_id && device_id[0]) {
		char encoded[sizeof ((player_state *)0)->device_id * 3];
		char *dst = encoded;
		const char *const end = encoded + sizeof encoded - 1;
		static const char hex[] = "0123456789ABCDEF";
		for (const unsigned char *src = (const unsigned char *)device_id;
		     *src && dst < end; src++) {
			const bool plain = (*src >= 'a' && *src <= 'z') ||
			                   (*src >= 'A' && *src <= 'Z') ||
			                   (*src >= '0' && *src <= '9') || *src == '-' ||
			                   *src == '_' || *src == '.' || *src == '~';
			if (plain) {
				*dst++ = (char)*src;
			} else {
				if (end - dst < 3)
					break;
				*dst++ = '%';
				*dst++ = hex[*src >> 4];
				*dst++ = hex[*src & 15];
			}
		}
		*dst = '\0';
		snprintf(path, sizeof path,
		         "/v1/me/player/volume?volume_percent=%d&device_id=%s",
		         volume_percent, encoded);
	} else {
		snprintf(path, sizeof path,
		         "/v1/me/player/volume?volume_percent=%d", volume_percent);
	}
	return simple_cmd("PUT", path, err, errlen);
}

player_result player_shuffle(bool on, char *err, int errlen)
{
	char path[64];
	snprintf(path, sizeof path, "/v1/me/player/shuffle?state=%s",
	         on ? "true" : "false");
	return simple_cmd("PUT", path, err, errlen);
}

repeat_mode repeat_next(repeat_mode m)
{
	switch (m) {
		case REPEAT_OFF:     return REPEAT_CONTEXT;
		case REPEAT_CONTEXT: return REPEAT_TRACK;
		default:             return REPEAT_OFF;
	}
}

player_result player_play_context(const char *context_uri, char *err,
                                  int errlen)
{
	return player_play_context_at(context_uri, -1, err, errlen);
}

player_result player_play_context_at(const char *context_uri, int position,
                                     char *err, int errlen)
{
	if (!context_uri || !context_uri[0]) {
		snprintf(err, errlen, "no context uri");
		return PLAYER_ERROR;
	}

	char body[256];
	if (position >= 0)
		snprintf(body, sizeof body,
		         "{\"context_uri\":\"%s\",\"offset\":{\"position\":%d},"
		         "\"position_ms\":0}",
		         context_uri, position);
	else
		snprintf(body, sizeof body, "{\"context_uri\":\"%s\"}", context_uri);

	http_response r;
	const player_result pr = api_call("PUT", "/v1/me/player/play",
	                                  "application/json", body, &r, err, errlen);
	if (pr == PLAYER_OK || r.body)
		http_free(&r);
	return pr;
}

player_result player_play_context_item(const char *context_uri,
                                       const char *item_uri, char *err,
                                       int errlen)
{
	if (!context_uri || !context_uri[0] || !item_uri || !item_uri[0]) {
		snprintf(err, errlen, "no context or item uri");
		return PLAYER_ERROR;
	}

	char body[384];
	snprintf(body, sizeof body,
	         "{\"context_uri\":\"%s\",\"offset\":{\"uri\":\"%s\"},"
	         "\"position_ms\":0}",
	         context_uri, item_uri);

	http_response r;
	const player_result pr = api_call("PUT", "/v1/me/player/play",
	                                  "application/json", body, &r, err, errlen);
	if (pr == PLAYER_OK || r.body)
		http_free(&r);
	return pr;
}

player_result player_repeat(repeat_mode mode, char *err, int errlen)
{
	const char *s = mode == REPEAT_TRACK     ? "track"
	                : mode == REPEAT_CONTEXT ? "context"
	                                         : "off";
	char path[64];
	snprintf(path, sizeof path, "/v1/me/player/repeat?state=%s", s);
	return simple_cmd("PUT", path, err, errlen);
}
