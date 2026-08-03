#pragma once

#include <stdbool.h>

#include "spotify/player.h"

/* Background network thread.
 *
 * Every Spotify call takes 300ms-1.5s. Doing that on the render thread would
 * stall the UI for tens of frames and make the app feel broken, so all I/O
 * happens here and the main thread only ever touches a mutex-protected
 * snapshot.
 */

typedef enum {
	CMD_NONE = 0,
	CMD_PLAY,
	CMD_PAUSE,
	CMD_NEXT,
	CMD_PREV,
	CMD_SEEK,
	CMD_SHUFFLE,
} worker_cmd;

typedef struct {
	player_state  state;
	bool          have_state;
	player_result last_result;
	char          status[128];      /* human-readable status for the UI */
	char          status_hint[128]; /* what the user should do about it */
	bool          fatal;            /* setup problem, not a transient state */
	bool          busy;             /* a command or poll is in flight */
} worker_snapshot;

bool worker_start(char *err, int errlen);
void worker_stop(void);

/* Put the UI into the fatal state from the caller's side. Needed because
 * worker_start can fail before the thread ever runs, and the in-thread
 * set_fatal path would then never be reached - which is how a dead worker came
 * to render as the ordinary "Nothing playing" state. */
void worker_set_fatal(const char *what, const char *hint);

/* Queue a command. arg is position_ms for CMD_SEEK, 0/1 for CMD_SHUFFLE. */
void worker_post(worker_cmd cmd, long arg);

/* Copy the current state out under lock. Never blocks on network I/O. */
void worker_get(worker_snapshot *out);

/* Ask for a poll on the next worker tick (e.g. after a command). */
void worker_request_poll(void);
