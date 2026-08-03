#include "worker.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "spotify/auth.h"
#include "testlog.h"

#define WORKER_STACK (96 * 1024) /* TLS handshakes need room */
#define CMD_QUEUE    8

/* Poll cadence. Spotify's limit is roughly 180 req/min, so 3s while playing is
 * comfortable; back off when idle to save battery and requests. */
#define POLL_PLAYING_MS 3000
#define POLL_PAUSED_MS  10000
#define POLL_IDLE_MS    30000

static Thread    s_thread;
static LightLock s_lock;
static volatile bool s_quit;

/* Shared state, guarded by s_lock. */
static player_state  s_state;
static bool          s_have_state;
static player_result s_last_result;
static char          s_status[128];
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

	if (pr != PLAYER_OK && pr != PLAYER_NOTHING_PLAYING)
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
	if (!auth_load(err, sizeof err)) {
		set_status("no creds.cfg");
		tl_log("worker: %s", err);
		return;
	}
	if (!auth_token(err, sizeof err)) {
		set_status("auth failed");
		tl_log("worker: %s", err);
		return;
	}

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
	LightLock_Init(&s_lock);
	s_quit = false;

	/* Pin to core 1: the syscore. TLS crypto is heavy enough that sharing the
	 * app core with rendering would cost frames. */
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);

	s_thread = threadCreate(worker_main, NULL, WORKER_STACK, prio - 1, 1, true);
	if (!s_thread) {
		snprintf(err, errlen, "threadCreate failed");
		return false;
	}
	return true;
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
	LightLock_Lock(&s_lock);
	out->state       = s_state;
	out->have_state  = s_have_state;
	out->last_result = s_last_result;
	out->busy        = s_busy;
	snprintf(out->status, sizeof out->status, "%s", s_status);
	LightLock_Unlock(&s_lock);
}
