#pragma once

#include <stdbool.h>

typedef enum {
	PLAYER_OK = 0,
	PLAYER_NOTHING_PLAYING, /* 204: no active playback */
	PLAYER_NO_DEVICE,       /* 404: no active device to control */
	PLAYER_FORBIDDEN,       /* 403: usually a non-Premium account */
	PLAYER_AUTH_FAILED,     /* 401 that survived a token refresh */
	PLAYER_ERROR,           /* transport or unexpected status */
} player_result;

typedef struct {
	char track[192];
	char artist[192];
	char album[192];
	char art_url[256];
	long progress_ms;
	long duration_ms;
	bool is_playing;
	bool shuffle;
} player_state;

/* GET /v1/me/player/currently-playing */
player_result player_poll(player_state *out, char *err, int errlen);

/* Transport controls. All return PLAYER_OK on Spotify's 204. */
player_result player_play(char *err, int errlen);
player_result player_pause(char *err, int errlen);
player_result player_next(char *err, int errlen);
player_result player_prev(char *err, int errlen);
player_result player_seek(long position_ms, char *err, int errlen);
player_result player_shuffle(bool on, char *err, int errlen);

const char *player_result_str(player_result r);
