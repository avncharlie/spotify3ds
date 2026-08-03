#include "worker.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "spotify/auth.h"
#include "testlog.h"

#define WORKER_STACK (96 * 1024) /* TLS handshakes need room */
#define WORKER_CORE  0           /* see worker_start for why not core 1 */
#define CMD_QUEUE    8

/* Poll cadence. Spotify's limit is roughly 180 req/min, so 3s while playing is
 * comfortable; back off when idle to save battery and requests. */
#define POLL_PLAYING_MS 3000
#define POLL_PAUSED_MS  10000
#define POLL_IDLE_MS    30000

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
	player_result pr = player_poll(&st, err, sizeof err);

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

	switch (cmd) {
		case CMD_PLAY:    pr = player_play(err, sizeof err); break;
		case CMD_PAUSE:   pr = player_pause(err, sizeof err); break;
		case CMD_NEXT:    pr = player_next(err, sizeof err); break;
		case CMD_PREV:    pr = player_prev(err, sizeof err); break;
		case CMD_SEEK:    pr = player_seek(arg, err, sizeof err); break;
		case CMD_SHUFFLE: pr = player_shuffle(arg != 0, err, sizeof err); break;
		default: return;
	}

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
	u64 next_poll = osGetTime() + POLL_PLAYING_MS;

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

		/* After a command, poll shortly after so the optimistic UI reconciles
		 * with reality. Spotify needs a moment to apply it. */
		if (did_work)
			next_poll = osGetTime() + 1200;

		LightLock_Lock(&s_lock);
		const bool want_poll = s_poll_requested;
		s_poll_requested     = false;
		LightLock_Unlock(&s_lock);

		if (want_poll || osGetTime() >= next_poll) {
			LightLock_Lock(&s_lock);
			s_busy = true;
			const bool playing = s_have_state && s_state.is_playing;
			const bool have    = s_have_state;
			LightLock_Unlock(&s_lock);

			do_poll();

			u32 interval = playing ? POLL_PLAYING_MS
			               : have  ? POLL_PAUSED_MS
			                       : POLL_IDLE_MS;
			next_poll = osGetTime() + interval;
		}

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
