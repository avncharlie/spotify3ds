#include <3ds.h>
#include <3ds/3dslink.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/net.h"
#include "spotify/art.h"
#include "spotify/artcache.h"
#include "spotify/auth.h"
#include "spotify/player.h"
#include "testlog.h"
#include "ui/screen_player.h"
#include "ui/screen_top.h"
#include "ui/touch.h"
#include "ui/ui.h"
#include "worker.h"

#define PHASE 6

/* Screens */
#define TOP_W    400.0f
#define SCREEN_H 240.0f

/* Colours */
#define CLR_TEXT   C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define CLR_DIM    C2D_Color32(0xB0, 0xB0, 0xB0, 0xFF)
#define CLR_FAINT  C2D_Color32(0x70, 0x70, 0x70, 0xFF)
#define CLR_GREEN  C2D_Color32(0x1D, 0xB9, 0x54, 0xFF)
#define CLR_TRACK  C2D_Color32(0x45, 0x45, 0x45, 0xFF)
#define CLR_BTN    C2D_Color32(0x28, 0x28, 0x28, 0xFF)
#define CLR_BTN_ON C2D_Color32(0x3A, 0x3A, 0x3A, 0xFF)
#define CLR_BOT_BG C2D_Color32(0x0E, 0x0E, 0x0E, 0xFF)
/* Setup failures render in red so they cannot be mistaken for the idle state. */
#define CLR_ERROR     C2D_Color32(0xFF, 0x6B, 0x5B, 0xFF)
#define CLR_ERROR_DIM C2D_Color32(0xC0, 0x55, 0x4A, 0xFF)

/* Cover art placement */
#define ART_X 16.0f
#define ART_Y 20.0f
#define ART_D 200.0f

/* Hit rects are registered per frame by the drawing code (see touch.h). This
 * frame's set: */
static touch_builder g_tb;

/* Scrubber geometry (drawn, not the hit rect) */
/* Scrubber geometry comes from screen_player.h, so the bar the user drags is
 * the bar that was drawn. */

/* ---------------------------------------------------------------- state */

/* Optimistic overlay. Spotify takes 300ms-1.5s to reflect a command, so the
 * UI applies it locally at once and ignores contradicting polls briefly.
 * Without this the buttons feel dead. */
#define OPTIMISTIC_MS 2500

/* Scrubber drag state machine. While dragging we must ignore poll-driven
 * progress, or the playhead fights the finger every few seconds. */
typedef enum { SCRUB_IDLE, SCRUB_DRAGGING, SCRUB_COMMITTING } scrub_mode;
static scrub_mode g_scrub;
static long       g_scrub_ms;
static u64        g_scrub_until;
#define SCRUB_COMMIT_MS 3500

/* Local clock for interpolating progress between polls, so the bar moves
 * smoothly at 60fps rather than jumping every 3s. */
static long g_base_progress;
static u64  g_base_time;

static album_art g_art;

/* KEY_Y hides the cover and switches the top screen to the large-title
 * layout. The top screen has no digitizer, so this has to be a button. */
static bool g_art_hidden;

/* True when running under the headless harness, which needs the app to quit by
 * itself. On a real console the app must stay up until the user exits. */
static bool g_smoketest;

/* When the user last pressed next/prev, for measuring how long the cover takes
 * to catch up with the audio. */
static u64 g_cmd_sent;

/* One machine-readable block describing what this run is executing on, so a
 * transcript alone explains itself. rtc= in particular turns TLS certificate
 * validity from a hypothesis into an observation, and build= stops us chasing
 * bugs in a stale .3dsx. */
static void emit_banner(int link_fd)
{
	bool is_new3ds = false;
	APT_CheckNew3DS(&is_new3ds);

	tl_banner("build=%s %s new3ds=%d", __DATE__, __TIME__, (int)is_new3ds);

	char sysver[32] = "";
	if (R_SUCCEEDED(
	        osGetSystemVersionDataString(NULL, NULL, sysver, sizeof sysver)) &&
	    sysver[0])
		tl_banner("firmware=%s", sysver);

	/* Local time as the console sees it. mbedTLS validates notBefore/notAfter
	 * against this, so a wrong clock shows up here rather than as an opaque
	 * certificate error later. */
	time_t     now = time(NULL);
	struct tm *tm  = gmtime(&now);
	if (tm)
		tl_banner("rtc=%04d-%02d-%02dT%02d:%02d:%02d epoch=%lld",
		          tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour,
		          tm->tm_min, tm->tm_sec, (long long)now);

	/* art.c needs a 256KB linear texture plus a malloc for the decoded JPEG,
	 * and Azahar is far more permissive about both than a real console. */
	tl_banner("linear_free=%u", (unsigned)linearSpaceFree());

	tl_banner("netload=%d link_fd=%d smoketest=%d", link_fd >= 0 ? 1 : 0,
	          link_fd, (int)g_smoketest);

	FILE *f = fopen("sdmc:/spotify/creds.cfg", "r");
	if (!f) {
		tl_banner("creds=MISSING path=sdmc:/spotify/creds.cfg");
	} else {
		char line[256];
		int  id = 0, rt = 0;
		while (fgets(line, sizeof line, f)) {
			line[strcspn(line, "\r\n")] = '\0';
			if (strncmp(line, "client_id=", 10) == 0)
				id = (int)strlen(line) - 10;
			else if (strncmp(line, "refresh_token=", 14) == 0)
				rt = (int)strlen(line) - 14;
		}
		fclose(f);
		tl_banner("creds=found id_len=%d rt_len=%d", id, rt);
	}
}

/* Effective progress: the drag position while scrubbing, otherwise the last
 * poll plus elapsed wall time. */
static long effective_progress(const worker_snapshot *snap)
{
	if (g_scrub == SCRUB_DRAGGING || g_scrub == SCRUB_COMMITTING)
		return g_scrub_ms;

	if (!snap->have_state)
		return 0;

	long p = g_base_progress;
	if (snap->state.is_playing)
		p += (long)(osGetTime() - g_base_time);

	if (snap->state.duration_ms > 0 && p > snap->state.duration_ms)
		p = snap->state.duration_ms;
	return p;
}

/* Optimistic overlay, one implementation for all three toggles.
 *
 * Spotify takes 300ms-1.5s to reflect a command, so a tap applies locally at
 * once and the polled value is ignored until the hold expires. Without it the
 * buttons feel dead. Three near-identical copies of this was fine; a fourth
 * would not be. */
typedef struct {
	long value;
	u64  until;
} opt_field;

static opt_field g_opt_play, g_opt_shuf, g_opt_rep;

static void opt_set(opt_field *o, long v)
{
	o->value = v;
	o->until = osGetTime() + OPTIMISTIC_MS;
}

static long opt_get(const opt_field *o, long polled)
{
	return osGetTime() < o->until ? o->value : polled;
}

static bool effective_playing(const worker_snapshot *snap)
{
	return opt_get(&g_opt_play,
	               snap->have_state && snap->state.is_playing) != 0;
}

static bool effective_shuffle(const worker_snapshot *snap)
{
	return opt_get(&g_opt_shuf, snap->have_state && snap->state.shuffle) != 0;
}

static repeat_mode effective_repeat(const worker_snapshot *snap)
{
	const long polled = snap->have_state ? (long)snap->state.repeat : REPEAT_OFF;
	return (repeat_mode)opt_get(&g_opt_rep, polled);
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();
	ui_init(); /* derives the type scale from the system font; needs C2D up */

	tl_init(PHASE);

	/* Auto-exit is opt-in, so a real console runs until the user quits.
	 *   emulator: dev.sh touches sdmc:/spotify/.smoketest
	 *   hardware: dev.sh passes `3dslink -0 <target>-smoketest`, since
	 *             3dslink 0.6.3 can set argv[0] but no other argument. */
	{
		FILE *f = fopen("sdmc:/spotify/.smoketest", "r");
		if (f) {
			g_smoketest = true;
			fclose(f);
		}
	}
	if (argc > 0 && argv[0] && strstr(argv[0], "smoketest"))
		g_smoketest = true;

	/* Latency probes only during automated runs, so normal use stays quiet but
	 * regressions remain measurable. */
	tl_set_timing(g_smoketest);


	C3D_RenderTarget *top    = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

	C2D_TextBuf textbuf = C2D_TextBufNew(4096);

	char err[256];
	bool net_up = net_init(err, sizeof err);

	/* With `3dslink -s`, redirect stdout/stderr over the network to the host
	 * terminal. This is the only way to see diagnostics from real hardware,
	 * since 3dslink cannot read files back off the SD card. Requires sockets,
	 * so it must follow net_init(). Harmless when not netloaded. */
	if (net_up) {
		int lfd = link3dsStdio();
		emit_banner(lfd);

		/* Must come after link3dsStdio or the numbers only reach the SD log
		 * and never the host. */
		if (g_smoketest) {
			artcache_probe();
			ui_font_probe();
		}
	}

	if (!net_up) {
		tl_step("net_init", 0, "%s", err);
		worker_set_fatal("No network", "Check the console's WiFi connection");
	} else if (!worker_start(err, sizeof err)) {
		tl_step("worker_start", 0, "%s", err);
		/* Without this the UI would render a dead worker as the ordinary
		 * "Nothing playing" state, which is what made this fail silently. */
		worker_set_fatal("Internal error", err);
	} else {
		artcache_init();
		tl_step("worker_start", 1, "network thread started");
	}

	touch_state     touch = {.press_id = -1, .clicked = -1};
	worker_snapshot snap  = {0};
	char            last_art[256] = "";

	long last_seen_progress = -1;
	int  frames             = 0;
	bool logged_first       = false;
	u64  repeat_probe_at    = 0;
	repeat_mode repeat_probe_from = REPEAT_OFF;

	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		/* Y hides the cover. The top screen has no touch digitizer, so the
		 * art-off layout needs a physical button; every key but START was
		 * free. */
		if (hidKeysDown() & KEY_Y)
			g_art_hidden = !g_art_hidden;

		/* Exercise the art-hidden layout headlessly too, so 2A cannot rot
		 * unnoticed: flip it for a stretch in the middle of a smoketest. */
		if (g_smoketest)
			g_art_hidden = (frames > 480 && frames < 600);

		/* Hit rects come from the previous frame's draw, which is what the
		 * user was actually looking at when they touched. */
		touch_update(&touch, g_tb.rects, g_tb.n);
		tb_reset(&g_tb);
		worker_get(&snap);

		/* Re-base the interpolation clock whenever a poll brings new data. */
		if (snap.have_state && snap.state.progress_ms != last_seen_progress) {
			last_seen_progress = snap.state.progress_ms;
			g_base_progress    = snap.state.progress_ms;
			g_base_time        = osGetTime();

			/* A poll confirming our seek ends the commit hold. */
			if (g_scrub == SCRUB_COMMITTING) {
				const long d = snap.state.progress_ms - g_scrub_ms;
				if (d > -3000 && d < 6000)
					g_scrub = SCRUB_IDLE;
			}
		}
		/* ...and time out regardless, so a missed confirmation cannot wedge
		 * the scrubber. */
		if (g_scrub == SCRUB_COMMITTING && osGetTime() > g_scrub_until)
			g_scrub = SCRUB_IDLE;

		const bool playing  = effective_playing(&snap);
		const bool shuffled = effective_shuffle(&snap);
		const long progress = effective_progress(&snap);
		const long duration = snap.have_state ? snap.state.duration_ms : 0;

		/* --- input ---------------------------------------------------- */
		if (touch.pressed && touch.press_id == BTN_SCRUB && duration > 0)
			g_scrub = SCRUB_DRAGGING;

		if (g_scrub == SCRUB_DRAGGING && touch.down && duration > 0) {
			float f = ((float)touch.px - SCRUB_BAR_X) / SCRUB_BAR_W;
			if (f < 0.0f)
				f = 0.0f;
			if (f > 1.0f)
				f = 1.0f;
			g_scrub_ms = (long)(f * (float)duration);
		}

		if (g_scrub == SCRUB_DRAGGING && touch.released) {
			worker_post(CMD_SEEK, g_scrub_ms);
			g_scrub       = SCRUB_COMMITTING;
			g_scrub_until = osGetTime() + SCRUB_COMMIT_MS;
		}

		if (touch.clicked >= 0 && touch.clicked != BTN_SCRUB) {
			switch (touch.clicked) {
				case BTN_PLAY:
					opt_set(&g_opt_play, !playing);
					worker_post(playing ? CMD_PAUSE : CMD_PLAY, 0);
					break;
				case BTN_NEXT:
					g_cmd_sent = osGetTime();
					tl_timing("cmd NEXT at %llu",
					       (unsigned long long)g_cmd_sent);
					worker_post(CMD_NEXT, 0);
					break;
				case BTN_PREV:
					g_cmd_sent = osGetTime();
					tl_timing("cmd PREV at %llu",
					       (unsigned long long)g_cmd_sent);
					worker_post(CMD_PREV, 0);
					break;
				case BTN_SHUFFLE:
					opt_set(&g_opt_shuf, !shuffled);
					worker_post(CMD_SHUFFLE, !shuffled);
					break;
				case BTN_REPEAT: {
					/* Cycle all three states even though only two are drawn:
					 * the setting is shared with the user's other clients, and
					 * a two-state toggle would silently coerce a repeat-one
					 * set elsewhere into repeat-all. */
					const repeat_mode next =
					    repeat_next(effective_repeat(&snap));
					opt_set(&g_opt_rep, (long)next);
					worker_post(CMD_REPEAT, (long)next);
					break;
				}
				default:
					break;
			}
		}

		/* --- album art ------------------------------------------------ */
		/* Only ask; the worker does the ~1.5s of network and JPEG work. Doing
		 * it here used to freeze the render loop for that entire time. */
		if (snap.have_state && snap.state.art_url[0] &&
		    strcmp(snap.state.art_url, last_art) != 0) {
			tl_timing("art url changed (cmd->url %lldms)",
			          g_cmd_sent ? (long long)(osGetTime() - g_cmd_sent) : -1);
			worker_request_art(snap.state.art_url);
			snprintf(last_art, sizeof last_art, "%s", snap.state.art_url);
		}

		/* Claim a finished download. Only the GPU upload happens here, which is
		 * cheap enough to sit in the frame. */
		{
			art_payload art;
			if (worker_take_art(&art)) {
				char aerr[128];
				bool ok;

				if (art.from_cache) {
					/* Already tiled on disk, so this skips the Morton pass and
					 * the accent extraction as well as network and decode.
					 * art_upload_tiled takes ownership of the buffer. */
					ok = art_upload_tiled(&g_art, art.tiled, art.w, art.h,
					                      art.accent_r, art.accent_g,
					                      art.accent_b, art.url, aerr,
					                      sizeof aerr);
					if (ok)
						art.tiled = NULL; /* consumed */
				} else {
					ok = art_upload(&g_art, art.rgba, art.w, art.h, art.url,
					                aerr, sizeof aerr);
					if (ok)
						g_art.decode_ms = art.decode_ms;
				}

				if (ok)
					tl_timing("art visible: source=%s fetch=%ums decode=%ums "
					          "cache=%ums cmd->visible=%lldms",
					          art.from_cache ? "cache" : "net", art.fetch_ms,
					          art.decode_ms, art.cache_ms,
					          g_cmd_sent
					              ? (long long)(osGetTime() - g_cmd_sent)
					              : -1);
				else
					tl_log("art upload failed: %s", aerr);

				g_cmd_sent = 0;
				art_payload_free(&art);
			}
		}

		if (!logged_first && snap.have_state) {
			logged_first = true;
			tl_step("first_poll", 1, "%s - %s", snap.state.track,
			        snap.state.artist);

			/* Phase 9: prove the new fields parse and the repeat endpoint
			 * works, before phase 11 builds a button on top of them. */
			tl_step("device_parsed", snap.state.device_name[0] != '\0',
			        "name=%s type=%s", snap.state.device_name,
			        snap.state.device_type);
			tl_step("repeat_parsed", 1, "mode=%d effective=%d",
			        (int)snap.state.repeat, (int)effective_repeat(&snap));

			if (g_smoketest) {
				/* Round-trip repeat through its full cycle and back, so a 403
				 * or a rejected state shows up here rather than as a dead
				 * button later. */
				repeat_probe_from = snap.state.repeat;
				worker_post(CMD_REPEAT, (long)repeat_next(snap.state.repeat));
				repeat_probe_at = osGetTime();
			}
		}

		/* Did the repeat command actually take?
		 *
		 * Wait for the change rather than sampling once at a fixed deadline:
		 * the worker polls on its own 3s cadence, so a single check can easily
		 * land on a poll issued before Spotify applied the change and report a
		 * false failure. Succeed as soon as the new state is observed, and only
		 * fail if it never arrives. */
		if (repeat_probe_at) {
			const repeat_mode want = repeat_next(repeat_probe_from);
			const bool arrived = snap.have_state && snap.state.repeat == want;
			const bool expired = osGetTime() - repeat_probe_at > 12000;

			if (arrived || expired) {
				tl_step("repeat_cmd", arrived, "%d -> %d (wanted %d) after %llums",
				        (int)repeat_probe_from,
				        (int)(snap.have_state ? snap.state.repeat : REPEAT_OFF),
				        (int)want,
				        (unsigned long long)(osGetTime() - repeat_probe_at));
				/* Put it back where the user had it. */
				worker_post(CMD_REPEAT, (long)repeat_probe_from);
				repeat_probe_at = 0;
			}
		}

		C2D_TextBufClear(textbuf);

		/* --- top screen ------------------------------------------------ */
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

		C2D_TargetClear(top, C2D_Color32(0, 0, 0, 0xFF));
		C2D_SceneBegin(top);

		{
			const char *hint = NULL;
			if (snap.fatal)
				hint = snap.status_hint;
			else if (snap.last_result == PLAYER_NO_DEVICE)
				hint = "Start Spotify on a device";

			const screen_top_args ta = {
				.buf        = textbuf,
				.art        = &g_art,
				.art_hidden = g_art_hidden,
				.have_state = snap.have_state,
				.fatal      = snap.fatal,
				.track      = snap.state.track,
				.artist     = snap.state.artist,
				.album      = snap.state.album,
				.device     = snap.state.device_name,
				.status     = snap.status,
				.hint       = hint,
			};
			screen_top_draw(&ta);
		}

		/* --- bottom screen --------------------------------------------- */
		C2D_TargetClear(bottom, CLR_BOT_BG);
		C2D_SceneBegin(bottom);

		{
			screen_player_args pa = {
				.buf         = textbuf,
				.tb          = &g_tb,
				.playing     = playing,
				.shuffle     = shuffled,
				.repeat      = effective_repeat(&snap),
				.progress_ms = progress,
				.duration_ms = duration,
				.pressed_id  = touch.down ? touch.press_id : -1,
				.scrubbing   = g_scrub == SCRUB_DRAGGING,
			};
			screen_player_draw(&pa);
		}

		C3D_FrameEnd(0);

		/* The headless harness needs the app to exit on its own; a real console
		 * must not. So auto-exit is opt-in, enabled only by the presence of
		 * sdmc:/spotify/.smoketest (which dev.sh creates). */
		if (++frames == 700 && g_smoketest) {
			tl_step("ui_loop", 1, "%d frames, art=%d", frames,
			        (int)g_art.valid);
			tl_done();
			break;
		}
	}

	worker_stop();
	art_free(&g_art);
	net_exit();
	C2D_TextBufDelete(textbuf);
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return 0;
}
