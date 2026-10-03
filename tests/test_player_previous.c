#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "net/http.h"
#include "spotify/player.h"

typedef struct {
	const char *method, *path;
	int status;
} expected_request;
static expected_request requests[4];
static int count, sent, refreshes;

const char *auth_token(char *err, int errlen)
{
	(void)err;
	(void)errlen;
	return "test-token";
}
bool auth_refresh(char *err, int errlen)
{
	(void)err;
	(void)errlen;
	refreshes++;
	return true;
}
void tl_log(const char *format, ...) { (void)format; }

bool http_request(const char *host, const char *method, const char *path,
                   const char *bearer, const char *ctype, const char *body,
                   http_response *out, char *err, int errlen)
{
	(void)ctype;
	(void)err;
	(void)errlen;
	assert(strcmp(host, "api.spotify.com") == 0);
	assert(bearer && strcmp(bearer, "test-token") == 0 && !body);
	assert(sent < count);
	expected_request expected = requests[sent++];
	assert(strcmp(method, expected.method) == 0);
	assert(strcmp(path, expected.path) == 0);
	memset(out, 0, sizeof *out);
	out->status = expected.status;
	return true;
}
void http_free(http_response *r) { memset(r, 0, sizeof *r); }

static void reset(void) { count = sent = refreshes = 0; }
static void expect(const char *method, const char *path, int status)
{
	assert(count < 4);
	requests[count++] = (expected_request){method, path, status};
}
static void check(long progress, bool restart)
{
	reset();
	expect(restart ? "PUT" : "POST",
	       restart ? "/v1/me/player/seek?position_ms=0" : "/v1/me/player/previous", 204);
	bool restarted = !restart;
	char err[128];
	assert(player_prev(progress, &restarted, err, sizeof err) == PLAYER_OK);
	assert(sent == 1 && restarted == restart);
}

static void test_seek_prediction(void)
{
	player_state state = {.progress_ms = 90000, .duration_ms = 200000, .is_playing = true};
	strcpy(state.track_uri, "spotify:track:one");
	player_seek_prediction prediction = {0};
	player_seek_predict(&prediction, &state, 0, 1000);
	player_state poll = state; /* Spotify still reports the pre-restart position */
	player_seek_reconcile(&prediction, &poll, 1500);
	assert(prediction.pending && poll.progress_ms == 500);
	poll = state;
	player_seek_reconcile(&prediction, &poll, 2500);
	assert(prediction.pending && poll.progress_ms == 1500); /* keeps moving, no snap back */
	poll.progress_ms = 1700;
	player_seek_reconcile(&prediction, &poll, 3000);
	assert(!prediction.pending && poll.progress_ms == 1700);

	player_seek_predict(&prediction, &state, 60000, 4000);
	poll = state;
	player_seek_reconcile(&prediction, &poll, 4500);
	assert(prediction.pending && poll.progress_ms == 60500);
	/* A newer scrub supersedes the earlier accepted seek. */
	player_seek_predict(&prediction, &state, 30000, 5000);
	poll.progress_ms = 61000;
	player_seek_reconcile(&prediction, &poll, 5500);
	assert(prediction.pending && poll.progress_ms == 30500);
	strcpy(poll.track_uri, "spotify:track:two");
	poll.progress_ms = 200;
	player_seek_reconcile(&prediction, &poll, 6000);
	assert(!prediction.pending && poll.progress_ms == 200);

	state.is_playing = false;
	player_seek_predict(&prediction, &state, 60000, 7000);
	poll = state;
	player_seek_reconcile(&prediction, &poll, 9000);
	assert(prediction.pending && poll.progress_ms == 60000);
	poll = state;
	player_seek_reconcile(&prediction, &poll, 12000);
	assert(!prediction.pending && poll.progress_ms == 90000); /* bounded failure recovery */

	state.is_playing = true;
	player_seek_predict(&prediction, &state, 60000, 13000);
	poll = state;
	poll.is_playing = false;
	player_seek_reconcile(&prediction, &poll, 15000);
	assert(prediction.pending && poll.progress_ms == 62000);
	poll.progress_ms = 90000;
	player_seek_reconcile(&prediction, &poll, 16000);
	assert(prediction.pending && poll.progress_ms == 62000); /* pause freezes prediction */
	poll.progress_ms = 90000;
	player_seek_reconcile(&prediction, &poll, 18000);
	assert(!prediction.pending && poll.progress_ms == 90000); /* deadline was not extended */
}

int main(void)
{
	test_seek_prediction();
	check(0, false);
	check(2999, false);
	check(3000, true);
	check(60000, true);
	check(-1, false); /* unknown playback keeps the ordinary previous command */

	player_state state = {.progress_ms = 2400, .duration_ms = 200000, .is_playing = true};
	check(player_estimated_progress(&state, 1000, 1500), false); /* 2900ms */
	check(player_estimated_progress(&state, 1000, 1600), true);  /* exactly 3000ms */
	state.is_playing = false;
	check(player_estimated_progress(&state, 1000, 999999), false);
	state.is_playing = true;
	assert(player_estimated_progress(&state, 1000, 900) == 2400); /* clock rollback */
	assert(player_estimated_progress(&state, 1000, UINT64_MAX) == 200000);
	state.duration_ms = 0;
	assert(player_estimated_progress(&state, 0, UINT64_MAX) == LONG_MAX);
	state.progress_ms = -100;
	assert(player_estimated_progress(&state, 1000, 1000) == 0);

	/* The UI captures the choice before network latency can cross the 3s
	 * boundary. A local restart also makes an immediate second Back a skip. */
	state.progress_ms = 2999;
	state.duration_ms = 200000;
	long at_input = player_estimated_progress(&state, 1000, 1000);
	assert(player_estimated_progress(&state, 1000, 3000) >= 3000);
	check(at_input, false);
	reset();
	expect("PUT", "/v1/me/player/seek?position_ms=0", 204);
	expect("POST", "/v1/me/player/previous", 204);
	char tap_error[128];
	bool first_restart;
	assert(player_prev(60000, &first_restart, tap_error, sizeof tap_error) == PLAYER_OK);
	assert(first_restart);
	state.progress_ms = 0; /* local interpolation starts at the tap */
	long second_tap = player_estimated_progress(&state, 4000, 4200);
	assert(player_prev(second_tap, &first_restart, tap_error, sizeof tap_error) == PLAYER_OK);
	assert(!first_restart && sent == 2);

	reset();
	expect("PUT", "/v1/me/player/seek?position_ms=0", 403);
	char err[128];
	bool restarted = true;
	assert(player_prev(3000, &restarted, err, sizeof err) == PLAYER_FORBIDDEN);
	assert(!restarted && sent == 1); /* failed restart must not become a skip */

	reset();
	expect("PUT", "/v1/me/player/seek?position_ms=0", 401);
	expect("PUT", "/v1/me/player/seek?position_ms=0", 204);
	assert(player_prev(3000, &restarted, err, sizeof err) == PLAYER_OK);
	assert(restarted && refreshes == 1 && sent == 2);

	reset();
	expect("POST", "/v1/me/player/previous", 204);
	assert(player_prev(1000, NULL, err, sizeof err) == PLAYER_OK);
	puts("previous/seek: exact 3s boundary, interpolation, stale-response protection, API and error handling passed");
	return 0;
}
