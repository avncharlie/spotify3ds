#pragma once

#include <stdbool.h>

#include "spotify/lyrics.h"
#include "spotify/player.h"
#include "spotify/recents.h"
#include "spotify/tracks.h"
#include "spotify/tracks_search.h"

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
	CMD_QUEUE_ITEM,
	CMD_VOLUME,
} worker_cmd;

typedef struct {
	player_state  state;
	bool          have_state;
	player_result last_result;
	char          status[128];      /* human-readable status for the UI */
	char          status_hint[128]; /* what the user should do about it */
	char          status_detail[128]; /* raw underlying error, for bug reports */
	bool          fatal;            /* setup problem, not a transient state */
	bool          busy;             /* a command or poll is in flight */
	unsigned      poll_seq;         /* increments after each completed poll */
} worker_snapshot;

bool worker_start(char *err, int errlen);
void worker_stop(void);

/* Put the UI into the fatal state from the caller's side. Needed because
 * worker_start can fail before the thread ever runs, and the in-thread
 * set_fatal path would then never be reached - which is how a dead worker came
 * to render as the ordinary "Nothing playing" state. */
void worker_set_fatal(const char *what, const char *hint);

/* Queue a command. arg is position_ms for CMD_SEEK, 0/1 for CMD_SHUFFLE,
 * and a repeat_mode for CMD_REPEAT. Volume uses worker_set_volume so rapid
 * shoulder presses can be coalesced. */
void worker_post(worker_cmd cmd, long arg);

/* Queue a seek only for the named current track. Unlike worker_post(CMD_SEEK),
 * this reports a full command queue and is rejected if a track-changing
 * command has overtaken it before execution. */
bool worker_seek_track(long position_ms, const char *track_uri);

/* Set volume on the device represented by the latest poll. New pending volume
 * commands replace older ones so rapid stepping does not issue every midpoint. */
bool worker_set_volume(int volume_percent, const char *device_id);

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

/* Start a collection at a raw playback position. The URI and position are
 * stored in the command-ring entry, so later actions cannot overwrite them. */
bool worker_play_context_at(const char *context_uri, int position);

/* Start the context at the selected Spotify track URI. */
bool worker_play_context_item(const char *context_uri, const char *item_uri);

/* Add one Spotify track to the active device's playback queue. */
bool worker_queue_item(const char *item_uri);

/* --- collection tracks ------------------------------------------------ */

typedef enum {
	TRACKS_IDLE = 0,
	TRACKS_LOADING,
	TRACKS_READY,
	TRACKS_ERROR,
} tracks_state;

typedef struct {
	track_page    page;
	tracks_state  state;
	player_result result;
	unsigned      generation;
	char          error[160];
} worker_tracks_snapshot;

/* Newest request wins. Results from an older in-flight request are discarded
 * by generation rather than replacing the collection the user now sees. */
unsigned worker_request_tracks(const collection_item *collection, int offset);
void     worker_cancel_tracks(void);
void     worker_get_tracks(worker_tracks_snapshot *out);

typedef enum {
	TRACK_SEARCH_IDLE = 0,
	TRACK_SEARCH_LOADING,
	TRACK_SEARCH_READY,
	TRACK_SEARCH_ERROR,
} track_search_state;

typedef struct {
	track_search_state state;
	unsigned           generation;
	char               context_uri[128];
	char               query[TRACK_SEARCH_QUERY_MAX + 1];
	int                scanned;
	int                source_total;
	int                matched_total;
	int                retained_count;
	bool               truncated;
	bool               payload_ready;
	char               error[160];
} worker_track_search_status;

/* `partial` marks a snapshot published mid-scan. Later pages can outrank what
 * it holds, so the ordering is only final once the scan reports READY. */
typedef struct {
	track_search_results results;
	unsigned             generation;
	unsigned             sequence;
	bool                 partial;
	char                 context_uri[128];
	char                 query[TRACK_SEARCH_QUERY_MAX + 1];
} worker_track_search_payload;

void worker_track_search_payload_init(worker_track_search_payload *payload);
void worker_track_search_payload_free(worker_track_search_payload *payload);
void worker_track_search_payload_move(worker_track_search_payload *dst,
                                      worker_track_search_payload *src);

unsigned worker_request_track_search(const collection_item *collection,
                                     const char *query);
void worker_cancel_track_search(void);
void worker_get_track_search_status(worker_track_search_status *out);
bool worker_take_track_search(worker_track_search_payload *out);

/* --- lyrics ------------------------------------------------------------
 * Status is cheap to copy every frame. The dynamic document is kept in a
 * separate, single-owner payload and can only be claimed by moving it. */

typedef enum {
	WORKER_LYRICS_IDLE = 0,
	WORKER_LYRICS_LOADING,
	WORKER_LYRICS_READY,
	WORKER_LYRICS_ERROR,
} worker_lyrics_state;

typedef struct {
	worker_lyrics_state state;
	lyrics_result       result;
	unsigned            generation;
	char                track_uri[128];
	char                message[160];
	lyrics_fetch_phase  phase;
	size_t              bytes_received;
	size_t              bytes_total;
	bool                bytes_total_known;
	bool                payload_ready;
} worker_lyrics_status;

typedef struct {
	lyrics_doc doc;
	unsigned   generation;
	char       track_uri[128];
} worker_lyrics_payload;

/* Payloads are move-only: initialize before first use, free when finished,
 * and use move rather than assigning the struct. Move replaces dst and leaves
 * src initialized and empty. */
void worker_lyrics_payload_init(worker_lyrics_payload *payload);
void worker_lyrics_payload_free(worker_lyrics_payload *payload);
void worker_lyrics_payload_move(worker_lyrics_payload *dst,
	                            worker_lyrics_payload *src);

/* Newest request wins. Re-requesting the same URI while it is loading or ready
 * preserves that generation and any unclaimed payload. */
unsigned worker_request_lyrics(const char *track_uri, const char *track,
	                           const char *artist, const char *album,
	                           long duration_ms);
void     worker_cancel_lyrics(void);
void     worker_get_lyrics_status(worker_lyrics_status *out);

/* Claim the completed document without copying it. out must be initialized;
 * on success, any document it currently owns is replaced by the ready one. */
bool worker_take_lyrics(worker_lyrics_payload *out);

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
