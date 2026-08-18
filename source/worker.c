#include "worker.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spotify/art.h"
#include "spotify/artcache.h"
#include "spotify/auth.h"
#include "spotify/lyrics.h"
#include "spotify/recents.h"
#include "spotify/searchcache.h"
#include "spotify/searchindex.h"
#include "spotify/tracks.h"
#include "testlog.h"

#define WORKER_STACK (96 * 1024) /* TLS handshakes need room */
#define WORKER_CORE  0           /* see worker_start for why not core 1 */
#define CMD_QUEUE    8

/* Poll cadence. Spotify's limit is roughly 180 req/min, so 3s while playing is
 * comfortable; back off when idle to save battery and requests. */
#define POLL_PLAYING_MS 3000
#define POLL_PAUSED_MS  10000
#define POLL_IDLE_MS    30000

/* After a command, how many extra quick polls to spend waiting for the change
 * to show up before dropping back to the normal cadence. Each poll is itself a
 * ~750ms round trip, so this is a short window, not a busy loop. */
#define SETTLE_RETRIES  3
#define SETTLE_RETRY_MS 250
#define LYRICS_PROGRESS_STEP 1024
#define SUB_SEP " \xC2\xB7 "

static Thread    s_thread;
static LightLock s_lock;
static bool      s_lock_ready;
static volatile bool s_quit;

typedef struct {
	char     track_uri[128];
	char     track[192];
	char     artist[192];
	char     album[192];
	long     duration_ms;
	unsigned generation;
} lyrics_request;

typedef struct {
	unsigned           generation;
	lyrics_fetch_phase phase;
	size_t             last_received;
	size_t             last_total;
	bool               last_total_known;
	bool               initialized;
} lyrics_progress_context;

static lyrics_request         s_lyrics_want;
static bool                   s_lyrics_pending;
static unsigned               s_lyrics_generation;
static worker_lyrics_status   s_lyrics_status;
static worker_lyrics_payload  s_lyrics_ready;
static bool                   s_lyrics_have;

typedef struct {
	collection_item collection;
	char            query[TRACK_SEARCH_QUERY_MAX + 1];
	unsigned        generation;
} track_search_request;

typedef struct {
	collection_item      collection;
	char                 query[TRACK_SEARCH_QUERY_MAX + 1];
	unsigned             generation;
	int                  next_offset;
	int                  source_total;
	u64                  next_request_at;
	u64                  next_publish_at;
	unsigned             sequence;
	bool                 active;
	bool                 from_cache;
	bool                 rescan; /* replacing results already on screen */
	char                 snapshot[SEARCHINDEX_SNAPSHOT_MAX + 1];
	track_search_results results;
	searchindex_builder *builder;
} track_search_job;

static track_search_request        s_track_search_want;
static bool                        s_track_search_pending;
static unsigned                    s_track_search_generation;
static worker_track_search_status  s_track_search_status;
static worker_track_search_payload s_track_search_ready;
static bool                        s_track_search_have;
static track_search_job            s_track_search_job;

/* Searches are answered from the stored index and nothing is kept between
 * them. An in-memory copy alongside the card was faster by tens of
 * milliseconds against an eighteen-second scan, and every way the two could
 * disagree - one holding a version the other did not, one being evicted while
 * the other answered - was a bug. One copy cannot disagree with itself. */

/* Set when results were served from a corpus whose currency has not been
 * confirmed yet. Validation runs on a later tick rather than inline, because
 * it costs a request and the query that triggered it is routinely cancelled
 * before one completes - the user types on. */
static char            s_search_validate_uri[128];
/* The version the served index carried, captured when it answered: validation
 * runs later, by which point the index itself is long freed. */
static char            s_search_validate_snapshot[SEARCHINDEX_SNAPSHOT_MAX + 1];
/* Records in the index that answered. Compared against what Spotify reports
 * now, because the snapshot alone has been seen lagging an edit. */
static int             s_search_validate_count;
static collection_item s_search_validate_collection;
static char            s_search_validate_query[TRACK_SEARCH_QUERY_MAX + 1];
static unsigned        s_search_validate_generation;

/* The lock must be usable before worker_start runs, because main.c can go
 * fatal on a path that never starts the worker at all (e.g. net_init failing). */
static void ensure_lock(void)
{
	if (!s_lock_ready) {
		LightLock_Init(&s_lock);
		worker_lyrics_payload_init(&s_lyrics_ready);
		worker_track_search_payload_init(&s_track_search_ready);
		track_search_results_init(&s_track_search_job.results);
		s_lock_ready = true;
	}
}

/* Shared state, guarded by s_lock. */
static player_state  s_state;
static bool          s_have_state;
static player_result s_last_result;
static char          s_status[128];
static char          s_status_hint[128];
static char          s_status_detail[128];
static bool          s_fatal;
static bool          s_busy;
static unsigned      s_poll_seq;

/* Command ring, guarded by s_lock. */
typedef struct {
	worker_cmd cmd;
	long       arg;
	char       context_uri[128];
	char       item_uri[128];
	char       device_id[128];
	char       expected_track_uri[128];
	int        position;
} queued_cmd;

static queued_cmd s_queue[CMD_QUEUE];
static int  s_qhead, s_qtail;
static bool s_poll_requested;
static bool s_track_change_pending;

/* Album art in flight. s_art_want is what the UI asked for; s_art_ready holds a
 * finished download waiting to be claimed. Guarded by s_lock. */
static recent_list s_recents;
static bool        s_recents_wanted = true; /* fetch once at startup */
static u64         s_recents_at;            /* last successful fetch */
static u64         s_recents_attempt_at;    /* retry backoff after failures */
static bool        s_current_meta_pending;
static char        s_current_meta_attempted[128];
static bool        s_current_fallback;

static playlist_list s_playlists;
static bool          s_playlists_wanted = true; /* fetch once at startup */
static album_list    s_albums;
static bool          s_albums_wanted = true; /* fetch once at startup */
#define RECENTS_MIN_INTERVAL_MS 30000
#define RECENTS_REFRESH_MS      (5 * 60 * 1000)

static char        s_art_want[256];
static char        s_art_inflight[256];
static art_payload s_art_ready;
static bool        s_art_have;

/* Thumbnails for the shelf and the Library rows.
 *
 * A queue rather than the hero's single slot, because the UI asks for several
 * at once and they are all wanted. They are fetched strictly after the hero -
 * see the tick order in worker_main - so a shelf full of misses cannot delay
 * the cover the user is actually looking at. */
#define THUMB_QUEUE 16

static char        s_thumb_q[THUMB_QUEUE][256];
static int         s_thumb_n;
static art_payload s_thumb_ready;
static bool        s_thumb_have;

typedef struct {
	collection_item collection;
	int             offset;
	unsigned        generation;
} track_request;

static track_request          s_tracks_want;
static bool                   s_tracks_pending;
static unsigned               s_tracks_generation;
static worker_tracks_snapshot s_tracks;

static void set_status(const char *s)
{
	LightLock_Lock(&s_lock);
	snprintf(s_status, sizeof s_status, "%s", s);
	LightLock_Unlock(&s_lock);
}

/* A setup problem the user must fix, with the remedy. Unlike a transient
 * status this persists, because retrying will not help.
 *
 * `detail` is the raw underlying error. It is deliberately shown on screen as
 * well as logged: the hint is a guess at the cause, and when the guess is wrong
 * the detail is the only thing that lets a bug report name the real failure. */
static void set_fatal_detail(const char *what, const char *hint,
                             const char *detail)
{
	LightLock_Lock(&s_lock);
	snprintf(s_status, sizeof s_status, "%s", what);
	snprintf(s_status_hint, sizeof s_status_hint, "%s", hint);
	snprintf(s_status_detail, sizeof s_status_detail, "%s",
	         detail ? detail : "");
	s_fatal = true;
	LightLock_Unlock(&s_lock);
}

static void set_fatal(const char *what, const char *hint)
{
	set_fatal_detail(what, hint, NULL);
}

static void do_art(void);
static void do_recents(void);
static void do_playlists(void);
static void do_albums(void);
static bool do_tracks(void);
static void do_track_search(bool higher_priority_work);
static void do_search_validate(void);
static void track_search_publish(track_search_job *job, bool partial);
static void do_lyrics(void);
static void do_thumbs(void);
static void do_current_metadata(void);

/* Short label for logs. Uses the *tail* of the content hash, not the head:
 * every Spotify art URL begins "ab67616d0000b273...", so a leading prefix is
 * identical for every cover and makes different tracks look like the same
 * entry in a transcript. */
static const char *want_key8(const char *url)
{
	const char *slash = strrchr(url, '/');
	const char *seg   = slash ? slash + 1 : url;
	const size_t n    = strlen(seg);
	return n > 8 ? seg + n - 8 : seg;
}

static bool uri_is(const char *uri, const char *prefix)
{
	return uri && strncmp(uri, prefix, strlen(prefix)) == 0;
}

static const char *playback_collection_uri(const player_state *st,
	                                       bool *is_playlist)
{
	*is_playlist = uri_is(st->context_uri, "spotify:playlist:");
	if (*is_playlist)
		return st->context_uri;
	if (uri_is(st->context_uri, "spotify:album:"))
		return st->context_uri;
	return uri_is(st->album_uri, "spotify:album:") ? st->album_uri : NULL;
}

static void pin_recent_locked(const collection_item *item)
{
	int kept = 0;
	for (int i = 0; i < s_recents.count; i++) {
		if (strcmp(s_recents.items[i].context_uri, item->context_uri) != 0)
			s_recents.items[kept++] = s_recents.items[i];
	}
	if (kept >= RECENTS_MAX)
		kept = RECENTS_MAX - 1;
	memmove(&s_recents.items[1], &s_recents.items[0],
	        (size_t)kept * sizeof s_recents.items[0]);
	s_recents.items[0] = *item;
	s_recents.count = kept + 1;
}

/* Build and pin immediately from data already in memory. A playlist missing
 * from both loaded lists still gets a correct URI and temporary tile; metadata
 * is enriched later without delaying the poll/art critical path. */
static bool pin_current_locked(const player_state *st, bool use_recent_meta)
{
	bool is_playlist = false;
	const char *uri = playback_collection_uri(st, &is_playlist);
	if (!uri)
		return true;

	collection_item item;
	memset(&item, 0, sizeof item);
	bool resolved = false;
	if (is_playlist) {
		for (int i = 0; i < s_playlists.count; i++) {
			if (strcmp(s_playlists.items[i].context_uri, uri) == 0) {
				item = s_playlists.items[i];
				resolved = true;
				break;
			}
		}
		if (!resolved && use_recent_meta) {
			for (int i = 0; i < s_recents.count; i++) {
				if (strcmp(s_recents.items[i].context_uri, uri) == 0 &&
				    !(s_current_fallback && i == 0)) {
					item = s_recents.items[i];
					resolved = true;
					break;
				}
			}
		}
		if (!resolved) {
			snprintf(item.name, sizeof item.name, "%.127s",
			         st->track[0] ? st->track : "Current playlist");
			snprintf(item.subtitle, sizeof item.subtitle,
			         "Playlist" SUB_SEP "%.115s",
			         st->artist);
			snprintf(item.art_url, sizeof item.art_url, "%s", st->art_url);
			snprintf(item.context_uri, sizeof item.context_uri, "%s", uri);
			item.kind = COLLECTION_PLAYLIST;
		}
	} else {
		for (int i = 0; i < s_albums.count; i++) {
			if (strcmp(s_albums.items[i].context_uri, uri) == 0) {
				item = s_albums.items[i];
				resolved = true;
				break;
			}
		}
		if (!resolved) {
			snprintf(item.name, sizeof item.name, "%.127s", st->album);
			snprintf(item.subtitle, sizeof item.subtitle,
			         "Album" SUB_SEP "%.118s",
			         st->artist);
			snprintf(item.art_url, sizeof item.art_url, "%s", st->art_url);
			snprintf(item.context_uri, sizeof item.context_uri, "%s", uri);
			item.kind = COLLECTION_ALBUM;
			resolved = true;
		}
	}

	pin_recent_locked(&item);
	s_current_fallback = is_playlist && !resolved;
	return resolved;
}

static void update_current_meta_pending_locked(const player_state *st,
	                                           bool resolved)
{
	bool is_playlist = false;
	const char *uri = playback_collection_uri(st, &is_playlist);
	s_current_meta_pending = !resolved && is_playlist && uri &&
	                         strcmp(uri, s_current_meta_attempted) != 0;
}

static bool pop_cmd(queued_cmd *out)
{
	LightLock_Lock(&s_lock);
	bool got = s_qhead != s_qtail;
	if (got) {
		*out = s_queue[s_qhead];
		s_qhead = (s_qhead + 1) % CMD_QUEUE;
	}
	LightLock_Unlock(&s_lock);
	return got;
}

static void do_poll(void)
{
	char          err[256];
	player_state  st;
	const u64     t0 = osGetTime();
	player_result pr = player_poll(&st, err, sizeof err);

	tl_timing("poll http took %lldms", (long long)(osGetTime() - t0));

	LightLock_Lock(&s_lock);
	s_poll_seq++;
	s_last_result = pr;
	if (pr == PLAYER_OK) {
		s_state      = st;
		s_have_state = true;
		s_track_change_pending = false;
		s_status[0]  = '\0';
		update_current_meta_pending_locked(&st, pin_current_locked(&st, true));
	} else {
		if (pr == PLAYER_NOTHING_PLAYING)
			s_have_state = false;
		snprintf(s_status, sizeof s_status, "%s", player_result_str(pr));
	}
	LightLock_Unlock(&s_lock);

	/* Log every poll outcome, not just failures: "nothing playing" on screen
	 * could be a genuine 204 or a masked error, and on hardware this is the
	 * only way to tell them apart.
	 *
	 * Only when it says something new, though. A poll repeats every three
	 * seconds while a track plays, and logging each one buried everything
	 * else - seventy per cent of a five megabyte log was identical lines. An
	 * unchanged poll carries no information the previous one did not.
	 *
	 * Repeats are still summarised on the next change, and every twentieth is
	 * logged regardless, so a stalled poller still leaves a trail rather than
	 * looking the same as a dead one. */
	if (pr == PLAYER_OK) {
		/* Matches tl_log's own buffer, so a line that would be truncated
		 * there is compared in the same truncated form here. */
		static char     last[512];
		static unsigned repeats;
		char            now[512];
		snprintf(now, sizeof now,
		         "poll ok: %.80s - %.60s (playing=%d item=%.60s context=%.60s "
		         "device=%.40s volume=%ld supported=%d)",
		         st.track, st.artist, (int)st.is_playing, st.track_uri,
		         st.context_uri[0] ? st.context_uri : "-",
		         st.device_id[0] ? st.device_id : "-",
		         st.volume_known ? st.volume_percent : -1L,
		         (int)st.supports_volume);
		if (strcmp(now, last) == 0 && ++repeats % 20 != 0)
			return;
		if (repeats && strcmp(now, last) != 0) {
			tl_log("poll: unchanged x%u", repeats);
			repeats = 0;
		}
		snprintf(last, sizeof last, "%s", now);
		tl_log("%s", now);
	} else {
		tl_log("poll: %s (%s)", player_result_str(pr), err);
	}
}

static void do_cmd(const queued_cmd *q)
{
	char          err[256];
	player_result pr = PLAYER_OK;

	const u64 t0 = osGetTime();

	switch (q->cmd) {
		case CMD_PLAY:    pr = player_play(err, sizeof err); break;
		case CMD_PAUSE:   pr = player_pause(err, sizeof err); break;
		case CMD_NEXT:    pr = player_next(err, sizeof err); break;
		case CMD_PREV:    pr = player_prev(err, sizeof err); break;
		case CMD_QUEUE_ITEM:
			pr = player_queue_item(q->item_uri, err, sizeof err);
			break;
		case CMD_SEEK:
			if (q->expected_track_uri[0]) {
				LightLock_Lock(&s_lock);
				const bool current = s_have_state && !s_track_change_pending &&
				                     strcmp(q->expected_track_uri,
				                            s_state.track_uri) == 0;
				LightLock_Unlock(&s_lock);
				if (!current) {
					tl_log("seek dropped: track changed (wanted=%s)",
					       q->expected_track_uri);
					return;
				}
			}
			pr = player_seek(q->arg, err, sizeof err);
			break;
		case CMD_SHUFFLE: pr = player_shuffle(q->arg != 0, err, sizeof err); break;
		case CMD_REPEAT:  pr = player_repeat((repeat_mode)q->arg, err, sizeof err); break;
		case CMD_VOLUME:
			pr = player_set_volume((int)q->arg, q->device_id, err, sizeof err);
			break;
		case CMD_PLAY_CONTEXT:
			if (q->item_uri[0])
				pr = player_play_context_item(q->context_uri, q->item_uri, err,
				                              sizeof err);
			else
				pr = player_play_context_at(q->context_uri, q->position, err,
				                            sizeof err);
			break;
		default: return;
	}
	if (pr == PLAYER_OK &&
	    (q->cmd == CMD_NEXT || q->cmd == CMD_PREV ||
	     q->cmd == CMD_PLAY_CONTEXT)) {
		LightLock_Lock(&s_lock);
		s_track_change_pending = true;
		LightLock_Unlock(&s_lock);
	}

	tl_timing("cmd %d http took %lldms", (int)q->cmd,
	       (long long)(osGetTime() - t0));
	if (q->cmd == CMD_QUEUE_ITEM)
		tl_log("queue: %s item=%s", player_result_str(pr), q->item_uri);

	if (pr != PLAYER_OK) {
		tl_log("cmd %d: %s (%s)", (int)q->cmd, player_result_str(pr), err);
		set_status(player_result_str(pr));
	}
}

/* Turn an auth_token error into the right remedy.
 *
 * Every one of these used to render as "Check system date/time", which sent
 * users hunting a clock that was already correct while the real fault was a
 * dead network or a bad client_id (issue #2).
 *
 * The matched strings are the complete set reachable from auth_token: the
 * explicit ones in auth.c, net.c, http.c and tls.c, plus the mbedTLS text that
 * tls.c's describe() passes through verbatim for a failed handshake. Order
 * matters - the narrow cases (invalid_grant, a 429) sit above the broader ones
 * (any token http, any SSL) that would otherwise swallow them. Anything
 * unrecognised stays generic rather than guessing, since the detail line
 * carries the truth either way. */
static void auth_fail_fatal(const char *err)
{
	/* Revoked or rotated-away refresh token: no amount of retrying helps. */
	if (strstr(err, "invalid_grant")) {
		set_fatal_detail("Authorization expired",
		                 "Run bootstrap_auth.py again and replace creds.cfg",
		                 err);
		return;
	}

	/* tls.c reports certificate rejection as "verify flags=0x...". A wrong RTC
	 * is by far the most common way to land here, because it makes a valid
	 * certificate look not-yet-valid or expired. */
	if (strstr(err, "verify flags=")) {
		set_fatal_detail("Certificate rejected",
		                 "Set the console's date, time and year, then relaunch",
		                 err);
		return;
	}

	/* net.c: DNS, socket and connect failures - the console cannot reach
	 * accounts.spotify.com at all. */
	if (strstr(err, "resolve") || strstr(err, "connect ") ||
	    strstr(err, "socket errno") || strstr(err, "socInit")) {
		set_fatal_detail("Network unreachable",
		                 "Check the console's WiFi connection, then relaunch",
		                 err);
		return;
	}

	/* http.c: the connection came up but died mid-exchange. Usually transient,
	 * so relaunching is genuinely worth a try here. */
	if (strstr(err, "send failed") || strstr(err, "read failed") ||
	    strstr(err, "no headers") || strstr(err, "truncated body") ||
	    strstr(err, "bad chunked body") || strstr(err, "malformed status")) {
		set_fatal_detail("Connection lost",
		                 "Spotify could not be reached - relaunch to retry",
		                 err);
		return;
	}

	/* auth.c formats rejections as "token http <status> <error>". Split by
	 * status: only the 4xx family implicates the credentials, and saying
	 * "check client_id" for a 429 or a Spotify outage would send the user
	 * editing a file that is perfectly correct. */
	if (strstr(err, "token http")) {
		if (strstr(err, "token http 429")) {
			set_fatal_detail("Rate limited by Spotify",
			                 "Too many requests - wait a minute, then relaunch",
			                 err);
		} else if (strstr(err, "token http 5")) {
			set_fatal_detail("Spotify is unavailable",
			                 "Spotify's servers are failing - try again later",
			                 err);
		} else {
			set_fatal_detail("Spotify rejected the login",
			                 "Check client_id in creds.cfg, or rerun bootstrap_auth.py",
			                 err);
		}
		return;
	}

	/* tls.c hands through mbedTLS's own text for any handshake failure that is
	 * not a certificate rejection: "SSL - <reason> (-0xXXXX)". These are the
	 * ones a user can actually act on. An EOF or a fatal alert mid-handshake is
	 * the classic symptom of the connection being interfered with rather than
	 * anything wrong on the console. */
	if (strstr(err, "indicated an EOF") || strstr(err, "fatal alert")) {
		set_fatal_detail("Secure connection refused",
		                 "The network dropped the TLS connection - try another WiFi network",
		                 err);
		return;
	}

	/* No shared ciphersuite. Nothing to do with the console: we ship our own
	 * mbedTLS and trust store (sslc is only an RNG source here), so this means
	 * Spotify now requires something this build does not offer. App-side, and
	 * only a new build can fix it. */
	if (strstr(err, "no ciphersuites in common")) {
		set_fatal_detail("Incompatible encryption",
		                 "Spotify3DS needs a cipher this build lacks - report this",
		                 err);
		return;
	}

	/* Any remaining mbedTLS failure. Still better than the old catch-all: it
	 * names TLS as the layer that failed rather than blaming the clock. */
	if (strstr(err, "SSL - ") || strstr(err, "X509 - ") ||
	    strstr(err, "CTR_DRBG - ") || strstr(err, "ENTROPY - ")) {
		set_fatal_detail("Secure connection failed",
		                 "Could not establish TLS - check WiFi, then relaunch",
		                 err);
		return;
	}

	/* Out of memory, in the allocator or in mbedTLS/SOC's buffers. The console
	 * does not multitask, so there is nothing to close: what actually frees the
	 * linear heap is a cold boot, or launching as a CIA rather than via the
	 * Homebrew Launcher, which leaves less headroom. */
	if (strcmp(err, "oom") == 0 || strstr(err, "memalign") ||
	    strstr(err, "Memory allocation failed")) {
		set_fatal_detail("Out of memory",
		                 "Restart the console, then relaunch",
		                 err);
		return;
	}

	/* A 200 that verified against our pinned roots but carried no access_token.
	 * Not an interception: anything answering in Spotify's place would have
	 * failed certificate verification above. So either the response shape
	 * changed or the JSON did not parse - app-side either way, and only a new
	 * build can fix it. */
	if (strstr(err, "no access_token")) {
		set_fatal_detail("Unexpected reply from Spotify",
		                 "Spotify3DS could not read the token - report this",
		                 err);
		return;
	}

	/* tls.c's bare label for a failed RNG seed. Nothing the user can fix, but
	 * naming it beats "Auth failed" in a bug report. */
	if (strstr(err, "entropy")) {
		set_fatal_detail("Secure connection failed",
		                 "The console's RNG could not start - relaunch",
		                 err);
		return;
	}

	set_fatal_detail("Auth failed", "See the detail below, then relaunch", err);
}

static void worker_main(void *arg)
{
	(void)arg;

	char err[256];

	tl_log("worker: starting, local time = %llu", (unsigned long long)osGetTime());

	if (!auth_load(err, sizeof err)) {
		/* Overwhelmingly the first-run-on-hardware case: 3dslink copies only
		 * the .3dsx, so the credentials never reach the real SD card. */
		set_fatal("No credentials", "Copy creds.cfg to SD:/spotify/creds.cfg");
		tl_log("worker: auth_load FAILED: %s", err);
		return;
	}
	tl_log("worker: creds loaded ok");

	if (!auth_token(err, sizeof err)) {
		auth_fail_fatal(err);
		tl_log("worker: auth_token FAILED: %s", err);
		return;
	}
	tl_log("worker: token ok");

	do_poll();
	u64 next_poll   = osGetTime() + POLL_PLAYING_MS;
	int settle_left = 0;

	while (!s_quit) {
		queued_cmd cmd;
		bool       did_work = false;
		bool       settle_track = false;

		while (pop_cmd(&cmd)) {
			LightLock_Lock(&s_lock);
			s_busy = true;
			LightLock_Unlock(&s_lock);

			settle_track = settle_track || cmd.cmd == CMD_NEXT ||
			               cmd.cmd == CMD_PREV || cmd.cmd == CMD_PLAY_CONTEXT;
			do_cmd(&cmd);
			did_work = true;
		}

		/* After a command, reconcile as fast as Spotify will let us.
		 *
		 * This used to wait a flat 1200ms on the theory that Spotify needs a
		 * moment to apply the change. Measured, that guess was most of the
		 * cover-art latency: ~1000ms of pure idling on the critical path
		 * between tapping next and the new artwork appearing.
		 *
		 * A skip is usually reflected by the time the command's own HTTP
		 * response lands (that round trip is already ~870ms), so poll straight
		 * away. If the track has not changed yet, track_settle_retries below
		 * re-polls a couple of times rather than waiting out a fixed delay. */
		if (did_work) {
			next_poll  = 0; /* poll on this iteration */
			settle_left = settle_track ? SETTLE_RETRIES : 0;
		}

		LightLock_Lock(&s_lock);
		const bool want_poll = s_poll_requested;
		s_poll_requested     = false;
		LightLock_Unlock(&s_lock);

		if (want_poll || osGetTime() >= next_poll) {
			LightLock_Lock(&s_lock);
			s_busy = true;
			const bool playing = s_have_state && s_state.is_playing;
			const bool have    = s_have_state;
			char prev_track[sizeof s_state.track];
			snprintf(prev_track, sizeof prev_track, "%s", s_state.track);
			LightLock_Unlock(&s_lock);

			do_poll();

			/* If we polled to confirm a command but the track has not turned
			 * over yet, try again shortly instead of falling back to the full
			 * 3s cadence - that gap is what made the artwork lag the audio. */
			bool settling = false;
			if (settle_left > 0) {
				LightLock_Lock(&s_lock);
				const bool changed =
				    strcmp(prev_track, s_state.track) != 0;
				LightLock_Unlock(&s_lock);

				if (changed) {
					settle_left = 0;
				} else {
					settle_left--;
					settling = true;
				}
			}

			if (settling) {
				next_poll = osGetTime() + SETTLE_RETRY_MS;
			} else {
				u32 interval = playing ? POLL_PLAYING_MS
				               : have  ? POLL_PAUSED_MS
				                       : POLL_IDLE_MS;
				next_poll = osGetTime() + interval;
			}
		}

		/* Download art after polling, so a fresh URL from the poll above is
		 * picked up in the same iteration rather than 100ms later. */
		do_art();
		const bool did_tracks = do_tracks();
		do_track_search(did_work || did_tracks);
		/* Lowest priority: correctness catches up behind whatever the user is
		 * actively waiting on. */
		if (!did_work && !did_tracks)
			do_search_validate();
		do_lyrics();

		/* Lists last: the cover the user is looking at matters more than the
		 * shelf behind it. Playlists before recents so the name cache is warm
		 * when recents_fetch needs to label a playlist context. */
		do_playlists();
		do_albums();
		do_recents();
		do_current_metadata();

		/* Thumbnails last of all: they are decoration, and a shelf full of
		 * cache misses must never stand between a track change and the cover
		 * appearing. */
		do_thumbs();

		LightLock_Lock(&s_lock);
		s_busy = false;
		LightLock_Unlock(&s_lock);

		svcSleepThread(100ull * 1000 * 1000); /* 100ms */
	}
}

bool worker_start(char *err, int errlen)
{
	ensure_lock(); /* must precede any failure return: worker_set_fatal takes
	                * this lock */
	s_quit = false;

	/* Core 0, the application core.
	 *
	 * This thread used to be pinned to core 1 on the theory that TLS crypto
	 * would cost frames if it shared a core with rendering. That was never
	 * measured and is wrong: the thread sleeps 100ms per iteration and does
	 * real work once every 3s, a ~1-2% duty cycle. A handshake preempting the
	 * render loop every few seconds costs a couple of frames at worst.
	 *
	 * Core 1 is also the wrong place for it. It is the system core, hosting
	 * wireless and audio, and reaching it requires APT_SetAppCpuTimeLimit,
	 * which *reserves* a share of that core away from the OS for the app's
	 * lifetime - i.e. taking CPU from the networking core in order to run
	 * networking code. Hardware refuses the unprivileged attempt outright
	 * (threadCreate returns NULL) while Azahar allows it, so this only ever
	 * failed on a real console.
	 *
	 * The priority bump is what actually matters for responsiveness, and it is
	 * legal on core 0. If frame pacing is ever measurably a problem, lower this
	 * thread's priority or chunk the handshake - do not reserve the syscore. */
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	const s32 worker_prio = prio - 1;

	s_thread = threadCreate(worker_main, NULL, WORKER_STACK, worker_prio,
	                        WORKER_CORE, false);
	if (!s_thread) {
		snprintf(err, errlen, "threadCreate failed (core %d prio 0x%lX)",
		         WORKER_CORE, (unsigned long)worker_prio);
		return false;
	}

	tl_log("worker thread: core %d prio 0x%lX stack %d", WORKER_CORE,
	       (unsigned long)worker_prio, WORKER_STACK);
	return true;
}

void worker_set_fatal(const char *what, const char *hint)
{
	ensure_lock();
	set_fatal(what, hint);
}

void worker_stop(void)
{
	s_quit = true;
	/* Invalidate before joining so an in-flight provider cannot publish while
	 * shutdown waits for its cancellation checkpoints. This also releases an
	 * unclaimed ready document. */
	worker_cancel_lyrics();
	worker_cancel_track_search();
	if (s_thread) {
		threadJoin(s_thread, U64_MAX);
		threadFree(s_thread);
		s_thread = NULL;
	}
	track_search_results_free(&s_track_search_job.results);
	searchindex_builder_free(s_track_search_job.builder);
	s_track_search_job.builder = NULL;
	worker_track_search_payload_free(&s_track_search_ready);
}

static bool enqueue(const queued_cmd *q)
{
	bool queued = false;
	LightLock_Lock(&s_lock);
	int next = (s_qtail + 1) % CMD_QUEUE;
	if (next != s_qhead) { /* drop if full rather than block the UI */
		s_queue[s_qtail] = *q;
		s_qtail = next;
		queued = true;
	}
	LightLock_Unlock(&s_lock);
	return queued;
}

void worker_post(worker_cmd cmd, long arg)
{
	ensure_lock();
	queued_cmd q;
	memset(&q, 0, sizeof q);
	q.cmd = cmd;
	q.arg = arg;
	q.position = -1;
	enqueue(&q);
}

bool worker_seek_track(long position_ms, const char *track_uri)
{
	if (!track_uri || !track_uri[0])
		return false;
	ensure_lock();
	queued_cmd q;
	memset(&q, 0, sizeof q);
	q.cmd = CMD_SEEK;
	q.arg = position_ms;
	q.position = -1;
	snprintf(q.expected_track_uri, sizeof q.expected_track_uri, "%s",
	         track_uri);

	/* A held scrub can produce another target while the previous seek is still
	 * queued behind network work. Replace that pending target rather than
	 * building a stale seek backlog. */
	bool queued = false;
	int replace = -1;
	LightLock_Lock(&s_lock);
	for (int i = s_qhead; i != s_qtail; i = (i + 1) % CMD_QUEUE) {
		if (s_queue[i].cmd == CMD_NEXT || s_queue[i].cmd == CMD_PREV ||
		    s_queue[i].cmd == CMD_PLAY_CONTEXT)
			replace = -1;
		else if (s_queue[i].cmd == CMD_SEEK &&
		         strcmp(s_queue[i].expected_track_uri, track_uri) == 0)
			replace = i;
	}
	if (replace >= 0) {
		s_queue[replace] = q;
		queued = true;
	}
	if (!queued) {
		const int next = (s_qtail + 1) % CMD_QUEUE;
		if (next != s_qhead) {
			s_queue[s_qtail] = q;
			s_qtail = next;
			queued = true;
		}
	}
	LightLock_Unlock(&s_lock);
	return queued;
}

bool worker_set_volume(int volume_percent, const char *device_id)
{
	if (volume_percent < 0 || volume_percent > 100 || !device_id ||
	    !device_id[0])
		return false;

	ensure_lock();
	queued_cmd q;
	memset(&q, 0, sizeof q);
	q.cmd = CMD_VOLUME;
	q.arg = volume_percent;
	q.position = -1;
	snprintf(q.device_id, sizeof q.device_id, "%s", device_id);

	bool queued = false;
	LightLock_Lock(&s_lock);
	for (int i = s_qhead; i != s_qtail; i = (i + 1) % CMD_QUEUE) {
		if (s_queue[i].cmd == CMD_VOLUME) {
			s_queue[i] = q;
			queued = true;
			break;
		}
	}
	if (!queued) {
		const int next = (s_qtail + 1) % CMD_QUEUE;
		if (next != s_qhead) {
			s_queue[s_qtail] = q;
			s_qtail = next;
			queued = true;
		}
	}
	LightLock_Unlock(&s_lock);
	return queued;
}

void worker_request_art(const char *url)
{
	if (!url || !url[0])
		return;

	ensure_lock();
	LightLock_Lock(&s_lock);
	/* Newest request wins; an older in-flight download for a track we have
	 * already skipped past is not worth waiting for. */
	if (strcmp(s_art_want, url) != 0)
		snprintf(s_art_want, sizeof s_art_want, "%s", url);
	LightLock_Unlock(&s_lock);
}

bool worker_take_art(art_payload *out)
{
	ensure_lock();
	LightLock_Lock(&s_lock);
	const bool have = s_art_have;
	if (have) {
		*out       = s_art_ready;
		s_art_have = false;
		memset(&s_art_ready, 0, sizeof s_art_ready);
	}
	LightLock_Unlock(&s_lock);
	return have;
}

void worker_request_thumb(const char *url)
{
	if (!url || !url[0])
		return;

	ensure_lock();
	LightLock_Lock(&s_lock);

	/* Already queued, or already the one being worked on: do nothing. The UI
	 * re-asks every frame it draws a missing tile, so this is the common
	 * path. */
	bool known = false;
	for (int i = 0; i < s_thumb_n; i++) {
		if (strcmp(s_thumb_q[i], url) == 0) {
			known = true;
			break;
		}
	}

	if (!known && s_thumb_n < THUMB_QUEUE)
		snprintf(s_thumb_q[s_thumb_n++], sizeof s_thumb_q[0], "%s", url);

	LightLock_Unlock(&s_lock);
}

bool worker_take_thumb(art_payload *out)
{
	ensure_lock();
	LightLock_Lock(&s_lock);
	const bool have = s_thumb_have;
	if (have) {
		*out         = s_thumb_ready;
		s_thumb_have = false;
		memset(&s_thumb_ready, 0, sizeof s_thumb_ready);
	}
	LightLock_Unlock(&s_lock);
	return have;
}

/* One thumbnail per tick, and only when the previous one has been claimed.
 *
 * Deliberately unhurried: thumbs are decoration next to the hero cover, and
 * doing them one at a time keeps the worker responsive to a track change. */
static void do_thumbs(void)
{
	char want[256];

	LightLock_Lock(&s_lock);
	const bool busy = s_thumb_have || s_thumb_n == 0;
	int        left = 0;
	if (!busy) {
		snprintf(want, sizeof want, "%s", s_thumb_q[0]);
		/* Pop the front now: a failure should not retry forever, and the UI
		 * will re-queue it on a later frame if it still wants it. */
		s_thumb_n--;
		memmove(s_thumb_q[0], s_thumb_q[1],
		        (size_t)s_thumb_n * sizeof s_thumb_q[0]);
		left = s_thumb_n;
	}
	LightLock_Unlock(&s_lock);

	if (busy)
		return;

	art_payload p;
	memset(&p, 0, sizeof p);
	snprintf(p.url, sizeof p.url, "%s", want);

	u8      *tiled = NULL;
	int      cw = 0, ch = 0, cdim = 0;
	u8       ar = 0, ag = 0, ab = 0;
	unsigned read_ms = 0;

	if (artcache_load(want, &tiled, &cw, &ch, &cdim, &ar, &ag, &ab, &read_ms)) {
		p.tiled      = tiled;
		p.w          = cw;
		p.h          = ch;
		p.tex_dim    = cdim;
		p.accent_r   = ar;
		p.accent_g   = ag;
		p.accent_b   = ab;
		p.cache_ms   = read_ms;
		p.from_cache = true;
		tl_timing("thumb HIT %dx%d read=%ums (%d left)", cw, ch, read_ms, left);
	} else {
		unsigned char *rgba = NULL;
		int            w = 0, h = 0;
		unsigned       fetch_ms = 0, decode_ms = 0;
		char           err[128];

		if (!art_fetch_decode(want, ART_THUMB_PX, &rgba, &w, &h, &fetch_ms,
		                      &decode_ms, err, sizeof err)) {
			tl_log("thumb failed: %s", err);
			return;
		}

		tl_timing("thumb MISS %dx%d fetch=%ums decode=%ums (%d left)", w, h,
		          fetch_ms, decode_ms, left);

		p.rgba      = rgba;
		p.w         = w;
		p.h         = h;
		p.fetch_ms  = fetch_ms;
		p.decode_ms = decode_ms;

		/* Store before publishing, unlike the hero: nothing is waiting on a
		 * thumb appearing this instant, and doing it here keeps the pixels
		 * alive without a second copy. */
		album_art tmp;
		memset(&tmp, 0, sizeof tmp);
		art_accent_of(rgba, w, h, &tmp);
		artcache_store(want, rgba, w, h, tmp.accent_r, tmp.accent_g,
		               tmp.accent_b);
	}

	LightLock_Lock(&s_lock);
	art_payload_free(&s_thumb_ready);
	s_thumb_ready = p;
	s_thumb_have  = true;
	LightLock_Unlock(&s_lock);
}

/* Runs on the worker thread: does the ~1.5s of network and JPEG work that used
 * to block the render loop. */
static void do_art(void)
{
	char want[256];

	LightLock_Lock(&s_lock);
	snprintf(want, sizeof want, "%s", s_art_want);
	const bool already = (want[0] && strcmp(want, s_art_inflight) == 0);
	LightLock_Unlock(&s_lock);

	if (!want[0] || already)
		return;

	LightLock_Lock(&s_lock);
	snprintf(s_art_inflight, sizeof s_art_inflight, "%s", want);
	LightLock_Unlock(&s_lock);

	art_payload p;
	memset(&p, 0, sizeof p);
	snprintf(p.url, sizeof p.url, "%s", want);

	/* --- cache first -------------------------------------------------- */
	u8      *tiled = NULL;
	int      cw = 0, ch = 0;
	u8       ar = 0, ag = 0, ab = 0;
	unsigned read_ms = 0;

	int cdim = 0;
	if (artcache_load(want, &tiled, &cw, &ch, &cdim, &ar, &ag, &ab, &read_ms)) {
		p.tiled      = tiled;
		p.w          = cw;
		p.h          = ch;
		p.tex_dim    = cdim;
		p.accent_r   = ar;
		p.accent_g   = ag;
		p.accent_b   = ab;
		p.cache_ms   = read_ms;
		p.from_cache = true;
		tl_timing("art cache HIT key=%.8s read=%ums", want_key8(want), read_ms);
	} else {
		unsigned char *rgba = NULL;
		int            w = 0, h = 0;
		unsigned       fetch_ms = 0, decode_ms = 0;
		char           err[128];

		if (!art_fetch_decode(want, ART_HERO_PX, &rgba, &w, &h, &fetch_ms,
		                      &decode_ms, err, sizeof err)) {
			tl_log("art failed: %s", err);
			return;
		}

		p.rgba      = rgba;
		p.w         = w;
		p.h         = h;
		p.fetch_ms  = fetch_ms;
		p.decode_ms = decode_ms;
		tl_timing("art cache MISS key=%.8s fetch=%ums decode=%ums",
		          want_key8(want), fetch_ms, decode_ms);
	}

	/* Keep our own copy of the pixels for the cache write. Once the payload is
	 * published the render thread owns and may free it at any moment, so it
	 * must not be read afterwards. */
	unsigned char *to_store = NULL;
	int            store_w = 0, store_h = 0;
	if (!p.from_cache && p.rgba) {
		const size_t n = (size_t)p.w * p.h * 4;
		to_store       = malloc(n);
		if (to_store) {
			memcpy(to_store, p.rgba, n);
			store_w = p.w;
			store_h = p.h;
		}
	}

	LightLock_Lock(&s_lock);
	/* If the user skipped again while this was loading, drop it. */
	if (strcmp(want, s_art_want) != 0) {
		LightLock_Unlock(&s_lock);
		art_payload_free(&p);
		free(to_store);
		return;
	}
	art_payload_free(&s_art_ready); /* discard an unclaimed older payload */
	s_art_ready = p;
	s_art_have  = true;
	LightLock_Unlock(&s_lock);

	/* Store only after publishing: a write costs ~140ms on hardware and must
	 * not sit between the download completing and the cover appearing. */
	if (to_store) {
		album_art tmp;
		memset(&tmp, 0, sizeof tmp);
		art_accent_of(to_store, store_w, store_h, &tmp);

		const u64 ts = osGetTime();
		artcache_store(want, to_store, store_w, store_h, tmp.accent_r,
		               tmp.accent_g, tmp.accent_b);
		tl_timing("art cache store=%lldms", (long long)(osGetTime() - ts));
		free(to_store);
	}
}

void art_payload_free(art_payload *p)
{
	if (!p)
		return;
	free(p->rgba);
	if (p->tiled)
		linearFree(p->tiled);
	p->rgba  = NULL;
	p->tiled = NULL;
}

int worker_get_recents(recent_list *out)
{
	ensure_lock();
	LightLock_Lock(&s_lock);
	*out = s_recents;
	const int n = s_recents.count;
	LightLock_Unlock(&s_lock);
	return n;
}

int worker_get_playlists(playlist_list *out)
{
	ensure_lock();
	LightLock_Lock(&s_lock);
	*out = s_playlists;
	const int n = s_playlists.count;
	LightLock_Unlock(&s_lock);
	return n;
}

void worker_request_playlists(void)
{
	ensure_lock();
	LightLock_Lock(&s_lock);
	s_playlists_wanted = true;
	LightLock_Unlock(&s_lock);
}

/* Runs on the worker thread.
 *
 * Fetched once per session rather than on a timer: a playlist library changes
 * far more slowly than playback state, and the response is 21KB. Ordered
 * *before* do_recents on purpose - it seeds the name cache with every playlist
 * the user owns or follows, so recents_fetch usually finds the names it needs
 * without a request of its own. */
static void do_playlists(void)
{
	LightLock_Lock(&s_lock);
	const bool want = s_playlists_wanted;
	LightLock_Unlock(&s_lock);

	if (!want)
		return;

	/* On the heap, not the stack: playlist_list is ~32KB and the worker runs on
	 * a 96KB stack that TLS handshakes already want most of. A stack copy here
	 * overflowed it and took the app down before it could log anything. */
	playlist_list *fresh = malloc(sizeof *fresh);
	if (!fresh)
		return;

	char                err[256];
	const player_result pr = playlists_fetch(fresh, err, sizeof err);

	LightLock_Lock(&s_lock);
	s_playlists_wanted = false;
	if (pr == PLAYER_OK) {
		s_playlists = *fresh;
		if (s_have_state)
			update_current_meta_pending_locked(
			    &s_state, pin_current_locked(&s_state, true));
	}
	LightLock_Unlock(&s_lock);

	free(fresh);

	if (pr != PLAYER_OK && pr != PLAYER_NOTHING_PLAYING)
		tl_log("playlists: %s (%s)", player_result_str(pr), err);
}

int worker_get_albums(album_list *out)
{
	ensure_lock();
	LightLock_Lock(&s_lock);
	*out = s_albums;
	const int n = s_albums.count;
	LightLock_Unlock(&s_lock);
	return n;
}

void worker_request_albums(void)
{
	ensure_lock();
	LightLock_Lock(&s_lock);
	s_albums_wanted = true;
	LightLock_Unlock(&s_lock);
}

static void do_albums(void)
{
	LightLock_Lock(&s_lock);
	const bool want = s_albums_wanted;
	LightLock_Unlock(&s_lock);

	if (!want)
		return;

	/* album_list is the same size class as playlist_list; keep it off the
	 * worker's TLS-constrained stack. */
	album_list *fresh = malloc(sizeof *fresh);
	if (!fresh)
		return;

	char                err[256];
	const player_result pr = albums_fetch(fresh, err, sizeof err);

	LightLock_Lock(&s_lock);
	s_albums_wanted = false;
	if (pr == PLAYER_OK) {
		s_albums = *fresh;
		if (s_have_state)
			update_current_meta_pending_locked(
			    &s_state, pin_current_locked(&s_state, true));
	}
	LightLock_Unlock(&s_lock);

	free(fresh);

	if (pr != PLAYER_OK && pr != PLAYER_NOTHING_PLAYING)
		tl_log("albums: %s (%s)", player_result_str(pr), err);
}

void worker_lyrics_payload_init(worker_lyrics_payload *payload)
{
	if (!payload)
		return;
	memset(payload, 0, sizeof *payload);
	lyrics_doc_init(&payload->doc);
}

void worker_lyrics_payload_free(worker_lyrics_payload *payload)
{
	if (!payload)
		return;
	lyrics_doc_free(&payload->doc);
	payload->generation = 0;
	payload->track_uri[0] = '\0';
}

void worker_lyrics_payload_move(worker_lyrics_payload *dst,
	                            worker_lyrics_payload *src)
{
	if (!dst || !src || dst == src)
		return;
	lyrics_doc_free(&dst->doc);
	lyrics_doc_move(&dst->doc, &src->doc);
	dst->generation = src->generation;
	snprintf(dst->track_uri, sizeof dst->track_uri, "%s", src->track_uri);
	src->generation = 0;
	src->track_uri[0] = '\0';
}

unsigned worker_request_lyrics(const char *track_uri, const char *track,
	                           const char *artist, const char *album,
	                           long duration_ms)
{
	if (!track_uri || !track_uri[0] || !track || !track[0])
		return 0;

	worker_lyrics_payload discarded;
	worker_lyrics_payload_init(&discarded);
	ensure_lock();
	LightLock_Lock(&s_lock);

	/* The render loop may ask every frame. Do not invalidate a completed
	 * document merely because it has not been claimed yet. Errors remain
	 * retryable by making another request. */
	if (strcmp(track_uri, s_lyrics_status.track_uri) == 0 &&
	    (s_lyrics_status.state == WORKER_LYRICS_LOADING ||
	     (s_lyrics_status.state == WORKER_LYRICS_READY && s_lyrics_have))) {
		const unsigned generation = s_lyrics_status.generation;
		LightLock_Unlock(&s_lock);
		worker_lyrics_payload_free(&discarded);
		return generation;
	}

	const unsigned generation = ++s_lyrics_generation;
	snprintf(s_lyrics_want.track_uri, sizeof s_lyrics_want.track_uri, "%s",
	         track_uri);
	snprintf(s_lyrics_want.track, sizeof s_lyrics_want.track, "%s", track);
	snprintf(s_lyrics_want.artist, sizeof s_lyrics_want.artist, "%s",
	         artist ? artist : "");
	snprintf(s_lyrics_want.album, sizeof s_lyrics_want.album, "%s",
	         album ? album : "");
	s_lyrics_want.duration_ms = duration_ms > 0 ? duration_ms : 0;
	s_lyrics_want.generation = generation;
	s_lyrics_pending = true;

	if (s_lyrics_have)
		worker_lyrics_payload_move(&discarded, &s_lyrics_ready);
	s_lyrics_have = false;
	s_lyrics_status.state = WORKER_LYRICS_LOADING;
	s_lyrics_status.result = (lyrics_result)0;
	s_lyrics_status.generation = generation;
	snprintf(s_lyrics_status.track_uri, sizeof s_lyrics_status.track_uri, "%s",
	         track_uri);
	s_lyrics_status.message[0] = '\0';
	s_lyrics_status.phase = LYRICS_FETCH_EXACT;
	s_lyrics_status.bytes_received = 0;
	s_lyrics_status.bytes_total = 0;
	s_lyrics_status.bytes_total_known = false;
	snprintf(s_lyrics_status.message, sizeof s_lyrics_status.message,
	         "Checking LRCLIB");
	s_lyrics_status.payload_ready = false;
	LightLock_Unlock(&s_lock);

	worker_lyrics_payload_free(&discarded);
	return generation;
}

void worker_cancel_lyrics(void)
{
	worker_lyrics_payload discarded;
	worker_lyrics_payload_init(&discarded);
	ensure_lock();
	LightLock_Lock(&s_lock);
	s_lyrics_generation++;
	s_lyrics_pending = false;
	memset(&s_lyrics_want, 0, sizeof s_lyrics_want);
	if (s_lyrics_have)
		worker_lyrics_payload_move(&discarded, &s_lyrics_ready);
	s_lyrics_have = false;
	s_lyrics_status.state = WORKER_LYRICS_IDLE;
	s_lyrics_status.result = (lyrics_result)0;
	s_lyrics_status.generation = s_lyrics_generation;
	s_lyrics_status.track_uri[0] = '\0';
	s_lyrics_status.message[0] = '\0';
	s_lyrics_status.phase = LYRICS_FETCH_EXACT;
	s_lyrics_status.bytes_received = 0;
	s_lyrics_status.bytes_total = 0;
	s_lyrics_status.bytes_total_known = false;
	s_lyrics_status.payload_ready = false;
	LightLock_Unlock(&s_lock);
	worker_lyrics_payload_free(&discarded);
}

void worker_get_lyrics_status(worker_lyrics_status *out)
{
	if (!out)
		return;
	ensure_lock();
	LightLock_Lock(&s_lock);
	*out = s_lyrics_status;
	LightLock_Unlock(&s_lock);
}

bool worker_take_lyrics(worker_lyrics_payload *out)
{
	if (!out)
		return false;

	worker_lyrics_payload claimed;
	worker_lyrics_payload_init(&claimed);
	ensure_lock();
	LightLock_Lock(&s_lock);
	const bool have = s_lyrics_have;
	if (have) {
		worker_lyrics_payload_move(&claimed, &s_lyrics_ready);
		s_lyrics_have = false;
		s_lyrics_status.payload_ready = false;
	}
	LightLock_Unlock(&s_lock);

	/* Releasing a caller-owned previous document can happen outside the shared
	 * lock. A failed take leaves out unchanged. */
	if (have)
		worker_lyrics_payload_move(out, &claimed);
	worker_lyrics_payload_free(&claimed);
	return have;
}

static bool lyrics_cancelled(void *ctx)
{
	if (s_quit)
		return true;

	const unsigned generation = *(const unsigned *)ctx;
	LightLock_Lock(&s_lock);
	const bool cancelled = generation != s_lyrics_generation;
	LightLock_Unlock(&s_lock);
	return cancelled;
}

static const char *lyrics_phase_message(lyrics_fetch_phase phase)
{
	switch (phase) {
		case LYRICS_FETCH_SEARCH: return "Searching LRCLIB";
		case LYRICS_FETCH_PROCESSING: return "Processing lyrics";
		case LYRICS_FETCH_EXACT:
		default: return "Checking LRCLIB";
	}
}

static void lyrics_progress(lyrics_fetch_phase phase, size_t received,
	                        size_t total, bool total_known, void *ctx)
{
	lyrics_progress_context *progress = ctx;
	const bool phase_changed = !progress->initialized ||
	                           phase != progress->phase;
	const bool reset = progress->initialized && phase == progress->phase &&
	                   received < progress->last_received;
	const bool total_changed = !progress->initialized ||
	                           total != progress->last_total ||
	                           total_known != progress->last_total_known;
	const bool first_data = progress->last_received == 0 && received > 0;
	const bool completed = total_known && received >= total;
	if (!phase_changed && !reset && !total_changed && !first_data && !completed &&
	    received < progress->last_received + LYRICS_PROGRESS_STEP)
		return;

	progress->phase = phase;
	progress->last_received = received;
	progress->last_total = total;
	progress->last_total_known = total_known;
	progress->initialized = true;

	LightLock_Lock(&s_lock);
	if (progress->generation == s_lyrics_generation &&
	    s_lyrics_status.state == WORKER_LYRICS_LOADING) {
		s_lyrics_status.phase = phase;
		s_lyrics_status.bytes_received = received;
		s_lyrics_status.bytes_total = total;
		s_lyrics_status.bytes_total_known = total_known;
		snprintf(s_lyrics_status.message, sizeof s_lyrics_status.message, "%s",
		         lyrics_phase_message(phase));
	}
	LightLock_Unlock(&s_lock);
}

static void do_lyrics(void)
{
	lyrics_request request;
	LightLock_Lock(&s_lock);
	const bool pending = s_lyrics_pending;
	if (pending) {
		request = s_lyrics_want;
		s_lyrics_pending = false;
		s_busy = true;
	}
	LightLock_Unlock(&s_lock);
	if (!pending)
		return;

	/* Keep the potentially large dynamic document off the TLS-constrained
	 * worker stack. The provider fills it without holding the shared lock. */
	lyrics_doc *fresh = malloc(sizeof *fresh);
	if (!fresh) {
		LightLock_Lock(&s_lock);
		if (request.generation == s_lyrics_generation) {
			s_lyrics_status.state = WORKER_LYRICS_ERROR;
			s_lyrics_status.result = LYRICS_ERR;
			s_lyrics_status.message[0] = '\0';
			snprintf(s_lyrics_status.message,
			         sizeof s_lyrics_status.message, "Out of memory");
			s_lyrics_status.payload_ready = false;
		}
		LightLock_Unlock(&s_lock);
		return;
	}
	lyrics_doc_init(fresh);

	char err[256] = "";
	lyrics_progress_context progress = {
		.generation = request.generation,
	};
	const lyrics_result result = lyrics_fetch_lrclib_progress(
	    request.track, request.artist, request.album, request.duration_ms, fresh,
	    lyrics_cancelled, &request.generation, lyrics_progress, &progress, err,
	    sizeof err);

	worker_lyrics_payload discarded;
	worker_lyrics_payload_init(&discarded);
	LightLock_Lock(&s_lock);
	const bool current = request.generation == s_lyrics_generation;
	if (current && result != LYRICS_CANCELLED) {
		s_lyrics_status.result = result;
		s_lyrics_status.generation = request.generation;
		snprintf(s_lyrics_status.track_uri,
		         sizeof s_lyrics_status.track_uri, "%s", request.track_uri);
		s_lyrics_status.payload_ready = false;

		if (s_lyrics_have)
			worker_lyrics_payload_move(&discarded, &s_lyrics_ready);
		s_lyrics_have = false;

		if (result == LYRICS_OK) {
			lyrics_doc_move(&s_lyrics_ready.doc, fresh);
			s_lyrics_ready.generation = request.generation;
			snprintf(s_lyrics_ready.track_uri,
			         sizeof s_lyrics_ready.track_uri, "%s", request.track_uri);
			s_lyrics_have = true;
			s_lyrics_status.state = WORKER_LYRICS_READY;
			s_lyrics_status.message[0] = '\0';
			s_lyrics_status.payload_ready = true;
		} else if (result == LYRICS_INSTRUMENTAL) {
			s_lyrics_status.state = WORKER_LYRICS_READY;
			snprintf(s_lyrics_status.message,
			         sizeof s_lyrics_status.message, "Instrumental track");
		} else if (result == LYRICS_NONE) {
			s_lyrics_status.state = WORKER_LYRICS_READY;
			snprintf(s_lyrics_status.message,
			         sizeof s_lyrics_status.message, "No lyrics found");
		} else {
			s_lyrics_status.state = WORKER_LYRICS_ERROR;
			snprintf(s_lyrics_status.message,
			         sizeof s_lyrics_status.message, "%s",
			         err[0] ? err : "Lyrics request failed");
		}
	}
	LightLock_Unlock(&s_lock);

	worker_lyrics_payload_free(&discarded);
	lyrics_doc_free(fresh);
	free(fresh);

	if (current && result == LYRICS_ERR)
		tl_log("lyrics: failed (%s)", err);
}

void worker_track_search_payload_init(worker_track_search_payload *payload)
{
	if (!payload)
		return;
	memset(payload, 0, sizeof *payload);
	track_search_results_init(&payload->results);
}

void worker_track_search_payload_free(worker_track_search_payload *payload)
{
	if (!payload)
		return;
	track_search_results_free(&payload->results);
	payload->generation = 0;
	payload->context_uri[0] = '\0';
	payload->query[0] = '\0';
}

void worker_track_search_payload_move(worker_track_search_payload *dst,
	                                  worker_track_search_payload *src)
{
	if (!dst || !src || dst == src)
		return;
	worker_track_search_payload_free(dst);
	track_search_results_move(&dst->results, &src->results);
	dst->generation = src->generation;
	dst->sequence = src->sequence;
	dst->partial = src->partial;
	snprintf(dst->context_uri, sizeof dst->context_uri, "%.127s",
	         src->context_uri);
	snprintf(dst->query, sizeof dst->query, "%.63s", src->query);
	src->generation = 0;
	src->sequence = 0;
	src->partial = false;
	src->context_uri[0] = '\0';
	src->query[0] = '\0';
}

unsigned worker_request_track_search(const collection_item *collection,
	                                 const char *query)
{
	if (!collection || !collection->context_uri[0] || !query || !query[0])
		return 0;
	worker_track_search_payload discarded;
	worker_track_search_payload_init(&discarded);
	ensure_lock();
	LightLock_Lock(&s_lock);
	const unsigned generation = ++s_track_search_generation;
	s_track_search_want.collection = *collection;
	snprintf(s_track_search_want.query, sizeof s_track_search_want.query, "%.63s",
	         query);
	s_track_search_want.generation = generation;
	s_track_search_pending = true;
	if (s_track_search_have)
		worker_track_search_payload_move(&discarded, &s_track_search_ready);
	s_track_search_have = false;
	memset(&s_track_search_status, 0, sizeof s_track_search_status);
	s_track_search_status.state = TRACK_SEARCH_LOADING;
	s_track_search_status.generation = generation;
	snprintf(s_track_search_status.context_uri,
	         sizeof s_track_search_status.context_uri, "%.127s",
	         collection->context_uri);
	snprintf(s_track_search_status.query, sizeof s_track_search_status.query,
	         "%.63s", query);
	LightLock_Unlock(&s_lock);
	worker_track_search_payload_free(&discarded);
	return generation;
}

/* Reissue a search only while `expect` is still the generation in force.
 * Checking that outside the lock and then calling the ordinary request would
 * leave a window where the user retypes in between: the request would then
 * overwrite their newer query with the older one, and because the render
 * thread matches incoming payloads against the query it is actually showing,
 * every result would be discarded and the search would never resolve. */
static bool reissue_track_search(const collection_item *collection,
	                             const char *query, unsigned expect)
{
	if (!collection || !collection->context_uri[0] || !query || !query[0])
		return false;
	worker_track_search_payload discarded;
	worker_track_search_payload_init(&discarded);
	ensure_lock();
	LightLock_Lock(&s_lock);
	const bool current = expect == s_track_search_generation;
	if (current) {
		const unsigned generation = ++s_track_search_generation;
		s_track_search_want.collection = *collection;
		snprintf(s_track_search_want.query, sizeof s_track_search_want.query,
		         "%.63s", query);
		s_track_search_want.generation = generation;
		s_track_search_pending = true;
		if (s_track_search_have)
			worker_track_search_payload_move(&discarded, &s_track_search_ready);
		s_track_search_have = false;
		memset(&s_track_search_status, 0, sizeof s_track_search_status);
		s_track_search_status.state = TRACK_SEARCH_LOADING;
		s_track_search_status.generation = generation;
		snprintf(s_track_search_status.context_uri,
		         sizeof s_track_search_status.context_uri, "%.127s",
		         collection->context_uri);
		snprintf(s_track_search_status.query,
		         sizeof s_track_search_status.query, "%.63s", query);
	}
	LightLock_Unlock(&s_lock);
	worker_track_search_payload_free(&discarded);
	return current;
}

void worker_cancel_track_search(void)
{
	worker_track_search_payload discarded;
	worker_track_search_payload_init(&discarded);
	ensure_lock();
	LightLock_Lock(&s_lock);
	s_track_search_generation++;
	s_track_search_pending = false;
	if (s_track_search_have)
		worker_track_search_payload_move(&discarded, &s_track_search_ready);
	s_track_search_have = false;
	memset(&s_track_search_status, 0, sizeof s_track_search_status);
	s_track_search_status.state = TRACK_SEARCH_IDLE;
	s_track_search_status.generation = s_track_search_generation;
	LightLock_Unlock(&s_lock);
	worker_track_search_payload_free(&discarded);
}

void worker_get_track_search_status(worker_track_search_status *out)
{
	if (!out)
		return;
	ensure_lock();
	LightLock_Lock(&s_lock);
	*out = s_track_search_status;
	LightLock_Unlock(&s_lock);
}

bool worker_take_track_search(worker_track_search_payload *out)
{
	if (!out)
		return false;
	worker_track_search_payload claimed;
	worker_track_search_payload_init(&claimed);
	ensure_lock();
	LightLock_Lock(&s_lock);
	/* Only hand over what the current request produced. The caller used to
	 * decide this from a status it had copied moments earlier, which let a
	 * request issued in between - the rescan after a playlist changes, most
	 * of all - publish under a newer generation than that copy knew about.
	 * The caller then rejected its own results and freed them, and since
	 * taking the payload clears it, nothing ever republished: the screen kept
	 * the stale list for good. Deciding here, under the lock that owns the
	 * generation, cannot go stale. */
	const bool have = s_track_search_have &&
	                  s_track_search_ready.generation == s_track_search_generation;
	/* A superseded payload is moved out too, so it cannot sit here blocking
	 * the results the newer request is about to publish. Either way the
	 * freeing happens outside the lock. */
	if (s_track_search_have) {
		worker_track_search_payload_move(&claimed, &s_track_search_ready);
		s_track_search_have = false;
		s_track_search_status.payload_ready = false;
	}
	LightLock_Unlock(&s_lock);
	if (have)
		worker_track_search_payload_move(out, &claimed);
	worker_track_search_payload_free(&claimed);
	return have;
}

static bool track_search_current(unsigned generation)
{
	LightLock_Lock(&s_lock);
	const bool current = generation == s_track_search_generation;
	LightLock_Unlock(&s_lock);
	return current;
}

/* Abandon a half-built index. A partial one must never be kept: it would look
 * complete and answer without the tracks the scan never reached. */
/* Answer straight out of a packed corpus. Returns false only if the search
 * could not be run at all, in which case the caller falls back to the network.
 * `validated` records whether the corpus was known current when it was used. */
static bool track_search_answer_from_index(track_search_job *job,
	                                       const searchindex *index)
{
	if (!searchindex_search(index, &job->collection, job->query, &job->results)) {
		track_search_results_free(&job->results);
		track_search_results_init(&job->results);
		return false;
	}
	job->source_total = searchindex_count(index);
	job->from_cache = true;
	LightLock_Lock(&s_lock);
	if (job->generation == s_track_search_generation) {
		s_track_search_status.scanned = job->source_total;
		s_track_search_status.source_total = job->source_total;
		s_track_search_status.matched_total = job->results.matched_total;
		s_track_search_status.retained_count = job->results.count;
		s_track_search_status.truncated = job->results.truncated;
		s_track_search_status.from_cache = true;
	}
	LightLock_Unlock(&s_lock);
	track_search_publish(job, false);
	track_search_results_free(&job->results);
	track_search_results_init(&job->results);
	return true;
}

static void track_search_drop_builder(track_search_job *job)
{
	searchindex_builder_free(job->builder);
	job->builder = NULL;
}

static void track_search_fail(track_search_job *job, player_result result,
	                          const char *error)
{
	LightLock_Lock(&s_lock);
	if (job->generation == s_track_search_generation) {
		s_track_search_status.state = TRACK_SEARCH_ERROR;
		s_track_search_status.error[0] = '\0';
		snprintf(s_track_search_status.error,
		         sizeof s_track_search_status.error, "%.159s",
		         error && error[0] ? error : player_result_str(result));
		s_track_search_status.payload_ready = false;
	}
	LightLock_Unlock(&s_lock);
	track_search_drop_builder(job);
	track_search_results_free(&job->results);
	job->active = false;
}

/* Publish what has matched so far. `results` stays owned by the job: the heap
 * keeps filling from later pages, so the snapshot is a sorted copy rather than
 * a move. Running out of memory here only costs this update, not the scan. */
static void track_search_publish(track_search_job *job, bool partial)
{
	track_search_results snapshot;
	track_search_results_init(&snapshot);
	if (partial) {
		if (!track_search_results_copy(&snapshot, &job->results))
			return;
		track_search_finalize(&snapshot);
	} else {
		track_search_finalize(&job->results);
		track_search_results_move(&snapshot, &job->results);
	}

	worker_track_search_payload discarded;
	worker_track_search_payload_init(&discarded);
	LightLock_Lock(&s_lock);
	if (job->generation == s_track_search_generation) {
		if (s_track_search_have)
			worker_track_search_payload_move(&discarded, &s_track_search_ready);
		s_track_search_have = false;
		track_search_results_move(&s_track_search_ready.results, &snapshot);
		s_track_search_ready.generation = job->generation;
		s_track_search_ready.sequence = ++job->sequence;
		s_track_search_ready.partial = partial;
		snprintf(s_track_search_ready.context_uri,
		         sizeof s_track_search_ready.context_uri, "%.127s",
		         job->collection.context_uri);
		snprintf(s_track_search_ready.query, sizeof s_track_search_ready.query,
		         "%.63s", job->query);
		s_track_search_have = true;
		if (!partial)
			s_track_search_status.state = TRACK_SEARCH_READY;
		s_track_search_status.payload_ready = true;
	}
	LightLock_Unlock(&s_lock);
	worker_track_search_payload_free(&discarded);
	track_search_results_free(&snapshot);
}

/* Confirm the corpus a recent search was answered from is still the version
 * Spotify holds, and rebuild it when it is not.
 *
 * Deliberately independent of any search generation: this outlives the query
 * that raised the question, so retyping does not keep cancelling the check.
 * The rebuild is a plain re-request, which the existing search path then
 * serves and re-indexes exactly as a cold search would. */
static void do_search_validate(void)
{
	if (!s_search_validate_uri[0])
		return;
	char uri[128];
	snprintf(uri, sizeof uri, "%.127s", s_search_validate_uri);
	s_search_validate_uri[0] = '\0';

	char name[128] = "", owner[128] = "", art[256] = "";
	char fresh[SEARCHINDEX_SNAPSHOT_MAX + 1] = "";
	int  live_total = -1;
	playlist_metadata(uri, name, sizeof name, owner, sizeof owner, art,
	                  sizeof art, fresh, sizeof fresh, &live_total);

	const char *held = s_search_validate_snapshot;
	const int   held_count = s_search_validate_count;
	/* Two signals, because neither is sufficient alone. The snapshot has been
	 * observed reporting the version from before a removal while the item
	 * count had already dropped - so a count that disagrees means the
	 * playlist changed even when the versions match. The snapshot still
	 * catches what the count cannot: a track swapped for another. */
	const bool count_differs =
	    live_total >= 0 && held_count >= 0 && live_total != held_count;
	tl_log("validate %s: fresh=%.10s held=%.10s total=%d/%d", name, fresh,
	       held, live_total, held_count);
	if (!fresh[0]) {
		/* Could not ask - offline, or the playlist is gone. Nothing to do:
		 * the stored index stays as it is and the next search checks again. */
		tl_log("search: could not verify %s, keeping stored index", name);
		return;
	}
	if (!held[0]) {
		/* Served an index with no version. Nothing is stored without one, so
		 * this should not happen; treating it as a change would rescan on
		 * every search and never settle, so leave it alone and say so. */
		tl_log("search: %s answered without a version, not rescanning", name);
		return;
	}
	if (strcmp(fresh, held) == 0 && !count_differs)
		return; /* current - nothing to do */

	tl_log("search: %s changed (%s), rescanning", name,
	       count_differs ? "item count" : "version");
	searchcache_evict(uri);

	/* Evicting is not enough. The search that used it has already
	 * The search that used it has already finished and handed back its
	 * results, so without reissuing it the user is left looking at the stale
	 * answer with nothing on screen to say so - and no amount of waiting
	 * corrects it.
	 *
	 * Only reissue while the user is still on the same query; if they have
	 * moved on, a newer request already governs. */
	if (s_search_validate_query[0]) {
		const bool sent = reissue_track_search(&s_search_validate_collection,
		                                       s_search_validate_query,
		                                       s_search_validate_generation);
		tl_log("search: reissue \"%s\" expect=%u sent=%d",
		       s_search_validate_query, s_search_validate_generation,
		       (int)sent);
	}
}

static void do_track_search(bool higher_priority_work)
{
	track_search_request request;
	LightLock_Lock(&s_lock);
	const bool pending = s_track_search_pending;
	if (pending) {
		request = s_track_search_want;
		s_track_search_pending = false;
	}
	LightLock_Unlock(&s_lock);
	if (pending) {
		/* Free before the memset, not after: it would otherwise zero the
		 * pointer to a builder the previous request left running and leak
		 * everything it had packed. */
		track_search_results_free(&s_track_search_job.results);
		searchindex_builder_free(s_track_search_job.builder);
		memset(&s_track_search_job, 0, sizeof s_track_search_job);
		track_search_results_init(&s_track_search_job.results);
		s_track_search_job.collection = request.collection;
		snprintf(s_track_search_job.query, sizeof s_track_search_job.query, "%.63s",
		         request.query);
		s_track_search_job.generation = request.generation;
		s_track_search_job.active = true;

		track_search_job *j = &s_track_search_job;

		/* Cache first, always. Results appear immediately whenever a corpus
		 * exists; whether it was still current is settled afterwards, and a
		 * stale one is corrected by the ordinary scan running underneath the
		 * results already on screen. The scan's own progress bar is what tells
		 * the user a refresh is happening, so there is no second indicator to
		 * keep in step. */
		bool served = false;
		int  served_count = -1;
		char served_snapshot[SEARCHINDEX_SNAPSHOT_MAX + 1] = "";
		searchindex *stored = searchcache_load(j->collection.context_uri);
		if (stored) {
			served = track_search_answer_from_index(j, stored);
			snprintf(served_snapshot, sizeof served_snapshot, "%s",
			         searchindex_snapshot(stored));
			served_count = searchindex_item_total(stored);
			/* Released straight away. Holding it would make refining a query
			 * cheaper, but it is the card's copy that gets validated and
			 * evicted, and a second copy outliving those is how the two came
			 * to disagree. */
			searchindex_free(stored);
		}

		/* Whether the stored index is still current is a property of the
		 * collection, not of this query. Checking it inline would tie a
		 * ~300ms request to a generation the user invalidates every time they
		 * retype, so it is deferred to the tick loop, where it can outlive the
		 * query that triggered it.
		 *
		 * Playlists only. An album is immutable and is never stored, so
		 * arming this for one would send a /v1/playlists/<album id> request
		 * that can only 404 - once per repeat search, forever, since asking
		 * for a snapshot deliberately bypasses the name cache. */
		if (served && j->collection.kind == COLLECTION_PLAYLIST) {
			snprintf(s_search_validate_uri, sizeof s_search_validate_uri,
			         "%.127s", j->collection.context_uri);
			s_search_validate_collection = j->collection;
			snprintf(s_search_validate_query, sizeof s_search_validate_query,
			         "%.63s", j->query);
			s_search_validate_generation = j->generation;
			snprintf(s_search_validate_snapshot,
			         sizeof s_search_validate_snapshot, "%s", served_snapshot);
			s_search_validate_count = served_count;
			tl_log("search: served %s from cache, armed snapshot=%.10s",
			       j->collection.name, served_snapshot);
			j->active = false;
			return;
		}

		if (j->collection.kind == COLLECTION_PLAYLIST) {
			/* Nothing cached. Take the snapshot before the walk, not after.
			 * The walk takes tens of seconds, and a playlist edited part-way
			 * through leaves the index holding pages from both sides of the
			 * edit. Stamped with the version from before, that index looks
			 * stale and is rebuilt on the next search - one wasted scan.
			 * Stamped with the version from after, it would look current
			 * forever while being wrong. Predating is the safe direction. */
			char name[128] = "", owner[128] = "", art[256] = "";
			playlist_metadata(j->collection.context_uri, name, sizeof name,
			                  owner, sizeof owner, art, sizeof art,
			                  j->snapshot, sizeof j->snapshot, NULL);
			if (!track_search_current(j->generation)) {
				j->active = false;
				return;
			}
		}

		j->builder = searchindex_builder_new(&j->collection, j->snapshot,
		                                     j->collection.item_total);
	}
	track_search_job *job = &s_track_search_job;
	if (!job->active)
		return;
	if (!track_search_current(job->generation)) {
		track_search_drop_builder(job);
		track_search_results_free(&job->results);
		job->active = false;
		return;
	}
	// We don't want to hammer Spotify with rapid playlist requests so we
	// wait 250ms between each one.
	if (higher_priority_work || osGetTime() < job->next_request_at)
		return;

	track_page *page = malloc(sizeof *page);
	if (!page) {
		track_search_fail(job, PLAYER_ERROR, "Out of memory");
		return;
	}
	char err[256] = "";
	const player_result result = tracks_fetch_page(
	    &job->collection, job->next_offset, page, err, sizeof err);
	if (!track_search_current(job->generation)) {
		// Cancel search if it has been superceded by another search
		free(page);
		track_search_drop_builder(job);
		track_search_results_free(&job->results);
		job->active = false;
		return;
	}
	if (result != PLAYER_OK) {
		free(page);
		if (strstr(err, "http 429")) {
			LightLock_Lock(&s_lock);
			if (job->generation == s_track_search_generation)
				snprintf(s_track_search_status.error,
				         sizeof s_track_search_status.error,
				         "Rate limited; retrying shortly");
			LightLock_Unlock(&s_lock);
			job->next_request_at = osGetTime() + 5000;
			return;
		}
		track_search_fail(job, result, err);
		return;
	}
	job->source_total = page->total;
	/* The pages carry the authoritative count; the one the builder started
	 * with came from the library listing and may predate an edit. */
	searchindex_builder_set_item_total(job->builder, page->total);
	const int matched_before = job->results.matched_total;
	if (!track_search_consider_page(
	        &job->results, page, job->query,
	        job->collection.kind == COLLECTION_PLAYLIST)) {
		free(page);
		track_search_fail(job, PLAYER_ERROR, "Out of memory retaining matches");
		return;
	}
	if (job->builder && !searchindex_builder_add_page(job->builder, page)) {
		/* Indexing is an optimisation; losing it must never cost the user
		 * their search. */
		searchindex_builder_free(job->builder);
		job->builder = NULL;
	}
	const bool matched_more = job->results.matched_total > matched_before;
	int next_offset = page->offset + page->count;
	if (next_offset <= job->next_offset && job->next_offset < page->total)
		next_offset = job->next_offset + TRACK_PAGE_MAX;
	if (next_offset > page->total)
		next_offset = page->total;
	job->next_offset = next_offset;
	const bool done = job->next_offset >= page->total || page->count == 0;
	free(page);

	LightLock_Lock(&s_lock);
	if (job->generation == s_track_search_generation) {
		s_track_search_status.error[0] = '\0';
		s_track_search_status.scanned = job->next_offset;
		s_track_search_status.source_total = job->source_total;
		s_track_search_status.matched_total = job->results.matched_total;
		s_track_search_status.retained_count = job->results.count;
		s_track_search_status.truncated = job->results.truncated;
	}
	LightLock_Unlock(&s_lock);
	if (!done) {
		const u64 now = osGetTime();
		/* Show matches as they are found instead of making the user wait out
		 * the whole collection, but rebuilding the page costs the render
		 * thread a scroll reset, so only refresh once a second. */
		if (matched_more && now >= job->next_publish_at) {
			track_search_publish(job, true);
			job->next_publish_at = now + 1000;
		}
		/* Paced rather than back-to-back, so a long scan does not monopolise
		 * the one network thread the poll and the artwork also queue behind.
		 * The wait starts after the fetch returns, and a page measures around
		 * 400ms, so the real gap between requests is that plus this. */
		job->next_request_at = now + 250;
		return;
	}

	track_search_publish(job, false);

	/* Keep the corpus for the next query on this collection. Installed only
	 * on a complete scan: a partial index would validate and then silently
	 * answer without the tracks it never saw. */
	if (job->builder) {
		unsigned char *blob = NULL;
		size_t         len = 0;
		if (searchindex_builder_finish(job->builder, &blob, &len)) {
			/* Only persist what can later be checked. An index built while
			 * the metadata request was failing carries no snapshot, and an
			 * unverifiable entry is worse than none: every later search would
			 * see a blank version, conclude the playlist had changed, and walk
			 * the whole thing again - amplifying load in exactly the
			 * conditions that made the request fail. This search still returns
			 * its results; the next one simply scans again.
			 *
			 * Written after the results are on screen, since the card costs
			 * ~140ms. */
			if (job->snapshot[0]) {
				searchcache_store(job->collection.context_uri, blob, len);
				tl_log("search: indexed %d tracks (%u bytes) for %s",
				       searchindex_builder_count(job->builder), (unsigned)len,
				       job->collection.name);
			}
			free(blob);
		}
		searchindex_builder_free(job->builder);
		job->builder = NULL;
	}

	track_search_results_free(&job->results);
	job->active = false;
}

unsigned worker_request_tracks(const collection_item *collection, int offset)
{
	if (!collection || !collection->context_uri[0])
		return 0;
	if (offset < 0)
		offset = 0;
	offset = (offset / TRACK_PAGE_MAX) * TRACK_PAGE_MAX;

	ensure_lock();
	LightLock_Lock(&s_lock);
	const unsigned generation = ++s_tracks_generation;
	s_tracks_want.collection = *collection;
	s_tracks_want.offset = offset;
	s_tracks_want.generation = generation;
	s_tracks_pending = true;

	memset(&s_tracks.page, 0, sizeof s_tracks.page);
	s_tracks.page.collection = *collection;
	s_tracks.page.offset = offset;
	s_tracks.state = TRACKS_LOADING;
	s_tracks.result = PLAYER_OK;
	s_tracks.generation = generation;
	s_tracks.error[0] = '\0';
	LightLock_Unlock(&s_lock);
	return generation;
}

void worker_cancel_tracks(void)
{
	ensure_lock();
	LightLock_Lock(&s_lock);
	s_tracks_generation++;
	s_tracks_pending = false;
	s_tracks.state = TRACKS_IDLE;
	s_tracks.generation = s_tracks_generation;
	memset(&s_tracks.page, 0, sizeof s_tracks.page);
	LightLock_Unlock(&s_lock);
}

void worker_get_tracks(worker_tracks_snapshot *out)
{
	ensure_lock();
	LightLock_Lock(&s_lock);
	*out = s_tracks;
	LightLock_Unlock(&s_lock);
}

static bool do_tracks(void)
{
	track_request request;
	LightLock_Lock(&s_lock);
	const bool pending = s_tracks_pending;
	if (pending) {
		request = s_tracks_want;
		s_tracks_pending = false;
		s_busy = true;
	}
	LightLock_Unlock(&s_lock);
	if (!pending)
		return false;

	track_page *page = malloc(sizeof *page);
	if (!page) {
		LightLock_Lock(&s_lock);
		if (request.generation == s_tracks_generation) {
			s_tracks.state = TRACKS_ERROR;
			s_tracks.result = PLAYER_ERROR;
			snprintf(s_tracks.error, sizeof s_tracks.error, "Out of memory");
		}
		LightLock_Unlock(&s_lock);
		return true;
	}

	char err[256] = "";
	const player_result pr = tracks_fetch_page(
	    &request.collection, request.offset, page, err, sizeof err);

	LightLock_Lock(&s_lock);
	if (request.generation == s_tracks_generation) {
		s_tracks.result = pr;
		s_tracks.generation = request.generation;
		if (pr == PLAYER_OK) {
			s_tracks.page = *page;
			s_tracks.state = TRACKS_READY;
			s_tracks.error[0] = '\0';
		} else {
			s_tracks.state = TRACKS_ERROR;
			snprintf(s_tracks.error, sizeof s_tracks.error, "%s",
			         err[0] ? err : player_result_str(pr));
		}
	}
	LightLock_Unlock(&s_lock);

	if (pr != PLAYER_OK)
		tl_log("tracks offset=%d: %s (%s)", request.offset,
		       player_result_str(pr), err);
	free(page);
	return true;
}

void worker_play_context(const char *context_uri)
{
	worker_play_context_at(context_uri, -1);
}

bool worker_play_context_at(const char *context_uri, int position)
{
	if (!context_uri || !context_uri[0])
		return false;
	ensure_lock();
	queued_cmd q;
	memset(&q, 0, sizeof q);
	q.cmd = CMD_PLAY_CONTEXT;
	q.position = position;
	snprintf(q.context_uri, sizeof q.context_uri, "%s", context_uri);
	return enqueue(&q);
}

bool worker_play_context_item(const char *context_uri, const char *item_uri)
{
	if (!context_uri || !context_uri[0] || !item_uri || !item_uri[0])
		return false;
	ensure_lock();
	queued_cmd q;
	memset(&q, 0, sizeof q);
	q.cmd = CMD_PLAY_CONTEXT;
	snprintf(q.context_uri, sizeof q.context_uri, "%s", context_uri);
	snprintf(q.item_uri, sizeof q.item_uri, "%s", item_uri);
	return enqueue(&q);
}

bool worker_queue_item(const char *item_uri)
{
	if (!item_uri || !item_uri[0])
		return false;
	ensure_lock();
	queued_cmd q;
	memset(&q, 0, sizeof q);
	q.cmd = CMD_QUEUE_ITEM;
	snprintf(q.item_uri, sizeof q.item_uri, "%s", item_uri);
	return enqueue(&q);
}

void worker_request_recents(void)
{
	ensure_lock();
	LightLock_Lock(&s_lock);
	s_recents_wanted = true;
	LightLock_Unlock(&s_lock);
}

/* Runs on the worker thread. Refreshes every five minutes while the app is
 * active; osGetTime also advances while the console sleeps, so a long lid-close
 * causes a refresh on the first worker pass after resume rather than any
 * network activity during sleep. Explicit requests remain debounced. */
static void do_recents(void)
{
	LightLock_Lock(&s_lock);
	const bool want = s_recents_wanted;
	const u64  last = s_recents_at;
	const u64  attempt = s_recents_attempt_at;
	LightLock_Unlock(&s_lock);

	const u64 now = osGetTime();
	if (attempt && now - attempt < RECENTS_MIN_INTERVAL_MS)
		return;
	if (!want && last && now - last < RECENTS_REFRESH_MS)
		return;

	/* Heap for the same reason as do_playlists: RECENTS_MAX grew from 8 to 16
	 * for the 50-item fetch, and 10KB of stack alongside a TLS handshake is not
	 * a margin worth relying on. */
	recent_list *fresh = malloc(sizeof *fresh);
	if (!fresh)
		return;

	char                err[256];
	const player_result pr = recents_fetch(fresh, err, sizeof err);

	LightLock_Lock(&s_lock);
	s_recents_wanted    = false;
	s_recents_attempt_at = osGetTime();
	if (pr == PLAYER_OK) {
		s_recents = *fresh;
		s_current_fallback = false;
		if (s_have_state)
			update_current_meta_pending_locked(
			    &s_state, pin_current_locked(&s_state, true));
		s_recents_at = s_recents_attempt_at;
	}
	LightLock_Unlock(&s_lock);

	free(fresh);

	if (pr != PLAYER_OK && pr != PLAYER_NOTHING_PLAYING)
		tl_log("recents: %s (%s)", player_result_str(pr), err);
}

static void do_current_metadata(void)
{
	player_state st;
	LightLock_Lock(&s_lock);
	const bool pending = s_current_meta_pending && s_have_state;
	if (pending)
		st = s_state;
	LightLock_Unlock(&s_lock);
	if (!pending)
		return;

	bool is_playlist = false;
	const char *uri = playback_collection_uri(&st, &is_playlist);
	if (!uri || !is_playlist) {
		LightLock_Lock(&s_lock);
		s_current_meta_pending = false;
		LightLock_Unlock(&s_lock);
		return;
	}

	collection_item item;
	memset(&item, 0, sizeof item);
	char owner[128] = "";
	const bool ok = playlist_metadata(uri, item.name, sizeof item.name, owner,
	                                  sizeof owner, item.art_url,
	                                  sizeof item.art_url, NULL, 0, NULL);
	if (ok) {
		snprintf(item.subtitle, sizeof item.subtitle,
		         "Playlist" SUB_SEP "%.115s",
		         owner[0] ? owner : st.artist);
		snprintf(item.context_uri, sizeof item.context_uri, "%s", uri);
		item.kind = COLLECTION_PLAYLIST;
	}

	LightLock_Lock(&s_lock);
	bool still_playlist = false;
	const char *current =
	    s_have_state ? playback_collection_uri(&s_state, &still_playlist) : NULL;
	if (current && still_playlist && strcmp(current, uri) == 0 && ok) {
		pin_recent_locked(&item);
		s_current_fallback = false;
	}
	snprintf(s_current_meta_attempted, sizeof s_current_meta_attempted, "%s",
	         uri);
	s_current_meta_pending = false;
	LightLock_Unlock(&s_lock);
}

void worker_request_poll(void)
{
	LightLock_Lock(&s_lock);
	s_poll_requested = true;
	LightLock_Unlock(&s_lock);
}

void worker_get(worker_snapshot *out)
{
	ensure_lock();
	LightLock_Lock(&s_lock);
	out->state       = s_state;
	out->have_state  = s_have_state;
	out->last_result = s_last_result;
	out->busy        = s_busy;
	out->fatal       = s_fatal;
	out->poll_seq    = s_poll_seq;
	snprintf(out->status, sizeof out->status, "%s", s_status);
	snprintf(out->status_hint, sizeof out->status_hint, "%s", s_status_hint);
	snprintf(out->status_detail, sizeof out->status_detail, "%s",
	         s_status_detail);
	LightLock_Unlock(&s_lock);
}
