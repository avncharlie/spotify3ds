#include "worker.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spotify/art.h"
#include "spotify/artcache.h"
#include "spotify/auth.h"
#include "spotify/recents.h"
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

static Thread    s_thread;
static LightLock s_lock;
static bool      s_lock_ready;
static volatile bool s_quit;

/* The lock must be usable before worker_start runs, because main.c can go
 * fatal on a path that never starts the worker at all (e.g. net_init failing). */
static void ensure_lock(void)
{
	if (!s_lock_ready) {
		LightLock_Init(&s_lock);
		s_lock_ready = true;
	}
}

/* Shared state, guarded by s_lock. */
static player_state  s_state;
static bool          s_have_state;
static player_result s_last_result;
static char          s_status[128];
static char          s_status_hint[128];
static bool          s_fatal;
static bool          s_busy;

/* Command ring, guarded by s_lock. */
static struct {
	worker_cmd cmd;
	long       arg;
} s_queue[CMD_QUEUE];
static int  s_qhead, s_qtail;
static bool s_poll_requested;

/* Album art in flight. s_art_want is what the UI asked for; s_art_ready holds a
 * finished download waiting to be claimed. Guarded by s_lock. */
static char        s_play_uri[128]; /* pending CMD_PLAY_CONTEXT target */

static recent_list s_recents;
static bool        s_recents_wanted = true; /* fetch once at startup */
static u64         s_recents_at;            /* last successful fetch */
static u64         s_recents_attempt_at;    /* retry backoff after failures */

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

static void set_status(const char *s)
{
	LightLock_Lock(&s_lock);
	snprintf(s_status, sizeof s_status, "%s", s);
	LightLock_Unlock(&s_lock);
}

/* A setup problem the user must fix, with the remedy. Unlike a transient
 * status this persists, because retrying will not help. */
static void set_fatal(const char *what, const char *hint)
{
	LightLock_Lock(&s_lock);
	snprintf(s_status, sizeof s_status, "%s", what);
	snprintf(s_status_hint, sizeof s_status_hint, "%s", hint);
	s_fatal = true;
	LightLock_Unlock(&s_lock);
}

static void do_art(void);
static void do_recents(void);
static void do_playlists(void);
static void do_albums(void);
static void do_thumbs(void);

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

static bool pop_cmd(worker_cmd *cmd, long *arg)
{
	LightLock_Lock(&s_lock);
	bool got = s_qhead != s_qtail;
	if (got) {
		*cmd    = s_queue[s_qhead].cmd;
		*arg    = s_queue[s_qhead].arg;
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
	s_last_result = pr;
	if (pr == PLAYER_OK) {
		s_state      = st;
		s_have_state = true;
		s_status[0]  = '\0';
	} else {
		if (pr == PLAYER_NOTHING_PLAYING)
			s_have_state = false;
		snprintf(s_status, sizeof s_status, "%s", player_result_str(pr));
	}
	LightLock_Unlock(&s_lock);

	/* Log every poll outcome, not just failures: "nothing playing" on screen
	 * could be a genuine 204 or a masked error, and on hardware this is the
	 * only way to tell them apart. */
	if (pr == PLAYER_OK)
		tl_log("poll ok: %s - %s (playing=%d)", st.track, st.artist,
		       (int)st.is_playing);
	else
		tl_log("poll: %s (%s)", player_result_str(pr), err);
}

static void do_cmd(worker_cmd cmd, long arg)
{
	char          err[256];
	player_result pr = PLAYER_OK;

	const u64 t0 = osGetTime();

	switch (cmd) {
		case CMD_PLAY:    pr = player_play(err, sizeof err); break;
		case CMD_PAUSE:   pr = player_pause(err, sizeof err); break;
		case CMD_NEXT:    pr = player_next(err, sizeof err); break;
		case CMD_PREV:    pr = player_prev(err, sizeof err); break;
		case CMD_SEEK:    pr = player_seek(arg, err, sizeof err); break;
		case CMD_SHUFFLE: pr = player_shuffle(arg != 0, err, sizeof err); break;
		case CMD_REPEAT:  pr = player_repeat((repeat_mode)arg, err, sizeof err); break;
		case CMD_PLAY_CONTEXT: {
			char uri[128];
			LightLock_Lock(&s_lock);
			snprintf(uri, sizeof uri, "%s", s_play_uri);
			LightLock_Unlock(&s_lock);
			pr = player_play_context(uri, err, sizeof err);
			break;
		}
		default: return;
	}

	tl_timing("cmd %d http took %lldms", (int)cmd,
	       (long long)(osGetTime() - t0));

	if (pr != PLAYER_OK) {
		tl_log("cmd %d: %s (%s)", (int)cmd, player_result_str(pr), err);
		set_status(player_result_str(pr));
	}
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
		/* On hardware this is usually clock skew: TLS rejects the certificate
		 * when the console's RTC is wrong. */
		set_fatal("Auth failed", "Check system date/time, then relaunch");
		tl_log("worker: auth_token FAILED: %s", err);
		return;
	}
	tl_log("worker: token ok");

	do_poll();
	u64 next_poll   = osGetTime() + POLL_PLAYING_MS;
	int settle_left = 0;

	while (!s_quit) {
		worker_cmd cmd;
		long       cmd_arg;
		bool       did_work = false;

		while (pop_cmd(&cmd, &cmd_arg)) {
			LightLock_Lock(&s_lock);
			s_busy = true;
			LightLock_Unlock(&s_lock);

			do_cmd(cmd, cmd_arg);
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
			settle_left = SETTLE_RETRIES;
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

		/* Lists last: the cover the user is looking at matters more than the
		 * shelf behind it. Playlists before recents so the name cache is warm
		 * when recents_fetch needs to label a playlist context. */
		do_playlists();
		do_albums();
		do_recents();

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
	                        WORKER_CORE, true);
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
	if (s_thread) {
		threadJoin(s_thread, U64_MAX);
		s_thread = NULL;
	}
}

void worker_post(worker_cmd cmd, long arg)
{
	LightLock_Lock(&s_lock);
	int next = (s_qtail + 1) % CMD_QUEUE;
	if (next != s_qhead) { /* drop if full rather than block the UI */
		s_queue[s_qtail].cmd = cmd;
		s_queue[s_qtail].arg = arg;
		s_qtail              = next;
	}
	LightLock_Unlock(&s_lock);
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
	if (pr == PLAYER_OK)
		s_playlists = *fresh;
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
	if (pr == PLAYER_OK)
		s_albums = *fresh;
	LightLock_Unlock(&s_lock);

	free(fresh);

	if (pr != PLAYER_OK && pr != PLAYER_NOTHING_PLAYING)
		tl_log("albums: %s (%s)", player_result_str(pr), err);
}

void worker_play_context(const char *context_uri)
{
	if (!context_uri || !context_uri[0])
		return;

	ensure_lock();
	LightLock_Lock(&s_lock);
	snprintf(s_play_uri, sizeof s_play_uri, "%s", context_uri);
	LightLock_Unlock(&s_lock);
	worker_post(CMD_PLAY_CONTEXT, 0);
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
		s_recents_at = s_recents_attempt_at;
	}
	LightLock_Unlock(&s_lock);

	free(fresh);

	if (pr != PLAYER_OK && pr != PLAYER_NOTHING_PLAYING)
		tl_log("recents: %s (%s)", player_result_str(pr), err);
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
	snprintf(out->status, sizeof out->status, "%s", s_status);
	snprintf(out->status_hint, sizeof out->status_hint, "%s", s_status_hint);
	LightLock_Unlock(&s_lock);
}
