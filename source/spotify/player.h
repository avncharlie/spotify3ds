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

/* Spotify's repeat is tri-state. The mockup only draws on/off, but the state is
 * shared with every other client the user has, so a two-state button would
 * silently coerce a repeat-one set on their phone into repeat-all. Cycle all
 * three; render `track` and `context` the same. */
typedef enum {
	REPEAT_OFF = 0,
	REPEAT_CONTEXT, /* repeat the album/playlist */
	REPEAT_TRACK,   /* repeat one */
} repeat_mode;

typedef struct {
	char track[192];
	char artist[192];
	char album[192];
	char art_url[256];
	char device_name[64];
	char device_type[32];
	long progress_ms;
	long duration_ms;
	bool is_playing;
	bool shuffle;
	repeat_mode repeat;
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
player_result player_repeat(repeat_mode mode, char *err, int errlen);

/* Next state in the off -> context -> track -> off cycle. */
repeat_mode repeat_next(repeat_mode m);

const char *player_result_str(player_result r);
