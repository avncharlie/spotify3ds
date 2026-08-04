#pragma once

#include <stdbool.h>

#include "spotify/player.h"
#include "spotify/recents.h"

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
	CMD_REPEAT,
	CMD_PLAY_CONTEXT,
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

/* Queue a command. arg is position_ms for CMD_SEEK, 0/1 for CMD_SHUFFLE,
 * and a repeat_mode for CMD_REPEAT. */
void worker_post(worker_cmd cmd, long arg);

/* Copy the current state out under lock. Never blocks on network I/O. */
void worker_get(worker_snapshot *out);

/* Ask for a poll on the next worker tick (e.g. after a command). */
void worker_request_poll(void);

/* --- recently played ---------------------------------------------------
 * Fetched at startup and every five minutes while the app is running. Manual
 * refresh requests are debounced so they cannot issue one request per skip. */

/* Copy the current list out under lock. Returns the item count. */
int worker_get_recents(recent_list *out);

/* Ask for a refresh on the next worker tick. */
void worker_request_recents(void);

/* --- playlist library --------------------------------------------------
 * The user's own and followed playlists, for the Library screen. Fetched once
 * at startup: it changes far more slowly than playback state. */

/* Copy the current list out under lock. Returns the item count. */
int worker_get_playlists(playlist_list *out);

/* Ask for a refresh on the next worker tick. */
void worker_request_playlists(void);

/* --- saved albums ------------------------------------------------------
 * The current user's saved album library, fetched once at startup. */
int  worker_get_albums(album_list *out);
void worker_request_albums(void);

/* Start playback from a recents entry. The uri is copied, so the caller's
 * buffer need not outlive the call. */
void worker_play_context(const char *context_uri);

/* --- album art -------------------------------------------------------
 * Fetching and decoding art costs ~1.5s, almost all of it network. Doing that
 * inline in the render loop froze the UI for that whole time, so the worker
 * owns the download and hands back a decoded RGBA buffer; only the (cheap) GPU
 * upload happens on the main thread, which is where it has to happen. */

typedef struct {
	/* Exactly one of these is ever non-NULL, and they need different frees:
	 * rgba is malloc'd (decoded, needs tiling), tiled is linearAlloc'd (from
	 * the SD cache, ready for the GPU). Always release via art_payload_free
	 * rather than freeing a field directly. */
	unsigned char *rgba;
	unsigned char *tiled;

	int      w, h;
	int      tex_dim; /* texture side for `tiled`; 0 when carrying rgba, which
	                   * is sized at upload time */
	unsigned fetch_ms;
	unsigned decode_ms;
	unsigned cache_ms;
	bool     from_cache;
	unsigned char accent_r, accent_g, accent_b;
	char     url[256];
} art_payload;

/* Release whichever buffer the payload holds and blank it. Safe on an empty
 * payload, and safe to call twice. */
void art_payload_free(art_payload *p);

/* Queue a fetch. Ignored if that URL is already loaded or in flight. */
void worker_request_art(const char *url);

/* Claim a completed download, if any. Returns false when nothing is ready.
 * On true, the caller owns payload->rgba and must free() it. */
bool worker_take_art(art_payload *out);

/* --- thumbnails --------------------------------------------------------
 * Shelf tiles and Library rows. Queued rather than single-slot, since the UI
 * wants several at once, and always fetched after the hero cover so a shelf of
 * misses cannot delay the art the user is looking at. */

/* Queue a thumbnail fetch. Ignored when already queued; safe to call every
 * frame for every missing tile, which is how the UI uses it. */
void worker_request_thumb(const char *url);

/* Claim one finished thumbnail. Same ownership rules as worker_take_art. */
bool worker_take_thumb(art_payload *out);
