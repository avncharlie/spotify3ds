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

/* Button ids */
enum { BTN_PREV = 0, BTN_PLAY, BTN_NEXT, BTN_SHUFFLE, BTN_SCRUB };

/* Hit rects, deliberately larger than the drawn artwork: the screen is
 * resistive and pressed with a thumb. */
static const touch_rect g_rects[] = {
	{  50.0f,  76.0f,  60.0f, 60.0f, BTN_PREV    },
	{ 124.0f,  66.0f,  72.0f, 80.0f, BTN_PLAY    },
	{ 210.0f,  76.0f,  60.0f, 60.0f, BTN_NEXT    },
	{ 132.0f, 200.0f,  56.0f, 40.0f, BTN_SHUFFLE },
	{  16.0f, 156.0f, 288.0f, 40.0f, BTN_SCRUB   },
};
#define NRECTS ((int)(sizeof g_rects / sizeof g_rects[0]))

/* Scrubber geometry (drawn, not the hit rect) */
#define BAR_X 20.0f
#define BAR_W 264.0f
#define BAR_Y 178.0f

/* ---------------------------------------------------------------- state */

/* Optimistic overlay. Spotify takes 300ms-1.5s to reflect a command, so the
 * UI applies it locally at once and ignores contradicting polls briefly.
 * Without this the buttons feel dead. */
static bool g_opt_playing;
static bool g_opt_shuffle;
static u64  g_opt_play_until;
static u64  g_opt_shuf_until;
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

static void fmt_time(long ms, char *out, int outlen)
{
	if (ms < 0)
		ms = 0;
	const long s = ms / 1000;
	snprintf(out, outlen, "%ld:%02ld", s / 60, s % 60);
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

static bool effective_playing(const worker_snapshot *snap)
{
	if (osGetTime() < g_opt_play_until)
		return g_opt_playing;
	return snap->have_state && snap->state.is_playing;
}

static bool effective_shuffle(const worker_snapshot *snap)
{
	if (osGetTime() < g_opt_shuf_until)
		return g_opt_shuffle;
	return snap->have_state && snap->state.shuffle;
}

/* ---------------------------------------------------------------- drawing */

static void tri_right(float x, float y, float size, u32 clr)
{
	C2D_DrawTriangle(x, y, clr, x, y + size, clr, x + size * 0.85f,
	                 y + size / 2, clr, 0.0f);
}

static void tri_left(float x, float y, float size, u32 clr)
{
	C2D_DrawTriangle(x + size * 0.85f, y, clr, x + size * 0.85f, y + size, clr,
	                 x, y + size / 2, clr, 0.0f);
}

static void draw_prev(float cx, float cy, u32 clr)
{
	tri_left(cx - 11.0f, cy - 10.0f, 20.0f, clr);
	C2D_DrawRectSolid(cx - 14.0f, cy - 10.0f, 0.0f, 3.0f, 20.0f, clr);
}

static void draw_next(float cx, float cy, u32 clr)
{
	tri_right(cx - 9.0f, cy - 10.0f, 20.0f, clr);
	C2D_DrawRectSolid(cx + 11.0f, cy - 10.0f, 0.0f, 3.0f, 20.0f, clr);
}

static void draw_playpause(float cx, float cy, bool playing, u32 clr)
{
	if (playing) {
		C2D_DrawRectSolid(cx - 9.0f, cy - 13.0f, 0.0f, 6.0f, 26.0f, clr);
		C2D_DrawRectSolid(cx + 3.0f, cy - 13.0f, 0.0f, 6.0f, 26.0f, clr);
	} else {
		tri_right(cx - 9.0f, cy - 13.0f, 26.0f, clr);
	}
}

/* Two interleaved arrows: enough to read as "shuffle" at this size. */
static void draw_shuffle(float cx, float cy, u32 clr)
{
	C2D_DrawRectSolid(cx - 12.0f, cy - 6.0f, 0.0f, 18.0f, 2.5f, clr);
	C2D_DrawRectSolid(cx - 12.0f, cy + 4.0f, 0.0f, 18.0f, 2.5f, clr);
	tri_right(cx + 5.0f, cy - 10.0f, 9.0f, clr);
	tri_right(cx + 5.0f, cy + 1.0f, 9.0f, clr);
}

/* Truncate with an ellipsis so long titles never overflow the panel. */
static void draw_text_fit(C2D_TextBuf buf, const char *s, float x, float y,
                          float scale, float maxw, u32 clr)
{
	if (!s || !s[0])
		return;

	char tmp[256];
	snprintf(tmp, sizeof tmp, "%s", s);

	C2D_Text t;
	C2D_TextParse(&t, buf, tmp);
	C2D_TextOptimize(&t);

	float w = 0.0f, h = 0.0f;
	C2D_TextGetDimensions(&t, scale, scale, &w, &h);

	/* Trim a character at a time. Cheap for a handful of labels, and avoids
	 * assuming a fixed glyph width in a proportional font. */
	int len = (int)strlen(tmp);
	while (w > maxw && len > 2) {
		len--;
		tmp[len - 1] = '.';
		tmp[len]     = '.';
		tmp[len + 1] = '\0';
		C2D_TextParse(&t, buf, tmp);
		C2D_TextOptimize(&t);
		C2D_TextGetDimensions(&t, scale, scale, &w, &h);
	}

	C2D_DrawText(&t, C2D_WithColor, x, y, 0.0f, scale, scale, clr);
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
	char            t_elapsed[16], t_total[16];

	long last_seen_progress = -1;
	int  frames             = 0;
	bool logged_first       = false;

	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;

		touch_update(&touch, g_rects, NRECTS);
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
			float f = ((float)touch.px - BAR_X) / BAR_W;
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
					g_opt_playing    = !playing;
					g_opt_play_until = osGetTime() + OPTIMISTIC_MS;
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
					g_opt_shuffle    = !shuffled;
					g_opt_shuf_until = osGetTime() + OPTIMISTIC_MS;
					worker_post(CMD_SHUFFLE, !shuffled);
					break;
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
		}

		C2D_TextBufClear(textbuf);

		/* --- top screen ------------------------------------------------ */
		const u32 wash = g_art.valid ? C2D_Color32(g_art.accent_r,
		                                           g_art.accent_g,
		                                           g_art.accent_b, 0xFF)
		                             : C2D_Color32(0x18, 0x18, 0x18, 0xFF);

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

		C2D_TargetClear(top, wash);
		C2D_SceneBegin(top);

		/* Vertical falloff to black, so the wash reads as depth rather than a
		 * flat slab of colour. */
		for (int i = 0; i < 8; i++) {
			const u8 a = (u8)(i * 18);
			C2D_DrawRectSolid(0.0f, SCREEN_H - (float)(8 - i) * 12.0f, 0.0f,
			                  TOP_W, 12.0f, C2D_Color32(0, 0, 0, a));
		}

		if (g_art.valid) {
			/* C2D_DrawImageAt scales relative to the subtexture's declared
			 * size (sub.width/height), not the full 256x256 texture, so the
			 * factor is target/subtex - not target/src. */
			const float sx = ART_D / (float)g_art.sub.width;
			const float sy = ART_D / (float)g_art.sub.height;
			C2D_DrawImageAt(g_art.image, ART_X, ART_Y, 0.0f, NULL, sx, sy);
		} else {
			C2D_DrawRectSolid(ART_X, ART_Y, 0.0f, ART_D, ART_D,
			                  C2D_Color32(0x22, 0x22, 0x22, 0xFF));
		}

		const float tx = ART_X + ART_D + 16.0f;
		const float tw = TOP_W - tx - 12.0f;

		if (snap.have_state) {
			draw_text_fit(textbuf, snap.state.track, tx, 44.0f, 0.62f, tw,
			              CLR_TEXT);
			draw_text_fit(textbuf, snap.state.artist, tx, 78.0f, 0.48f, tw,
			              CLR_DIM);
			draw_text_fit(textbuf, snap.state.album, tx, 104.0f, 0.40f, tw,
			              CLR_FAINT);
		} else {
			/* A setup failure must never look like the ordinary idle state.
			 * Showing "Nothing playing" for a dead worker is exactly what made
			 * the core-1 threadCreate failure invisible. */
			const char *primary =
			    snap.fatal ? snap.status
			               : (snap.status[0] ? snap.status : "Nothing playing");

			draw_text_fit(textbuf, primary, tx, 54.0f, 0.50f, tw,
			              snap.fatal ? CLR_ERROR : CLR_DIM);

			/* Say what to actually do about it. Diagnosing this on hardware
			 * without a console is otherwise painful. */
			const char *hint = NULL;
			if (snap.fatal)
				hint = snap.status_hint;
			else if (snap.last_result == PLAYER_NO_DEVICE)
				hint = "Start Spotify on a device";

			if (hint)
				draw_text_fit(textbuf, hint, tx, 84.0f, 0.34f, tw,
				              snap.fatal ? CLR_ERROR_DIM : CLR_FAINT);
		}

		/* --- bottom screen --------------------------------------------- */
		C2D_TargetClear(bottom, CLR_BOT_BG);
		C2D_SceneBegin(bottom);

		const bool press_play = touch.down && touch.press_id == BTN_PLAY;
		const bool press_prev = touch.down && touch.press_id == BTN_PREV;
		const bool press_next = touch.down && touch.press_id == BTN_NEXT;
		const bool press_shuf = touch.down && touch.press_id == BTN_SHUFFLE;

		C2D_DrawRectSolid(50.0f, 76.0f, 0.0f, 60.0f, 60.0f,
		                  press_prev ? CLR_BTN_ON : CLR_BTN);
		C2D_DrawRectSolid(210.0f, 76.0f, 0.0f, 60.0f, 60.0f,
		                  press_next ? CLR_BTN_ON : CLR_BTN);
		C2D_DrawRectSolid(124.0f, 66.0f, 0.0f, 72.0f, 80.0f,
		                  press_play ? CLR_BTN_ON : CLR_BTN);

		draw_prev(80.0f, 106.0f, CLR_TEXT);
		draw_next(240.0f, 106.0f, CLR_TEXT);
		draw_playpause(160.0f, 106.0f, playing, CLR_GREEN);

		/* Centred below the scrubber, clear of the time labels at either end. */
		if (press_shuf)
			C2D_DrawRectSolid(132.0f, 200.0f, 0.0f, 56.0f, 40.0f, CLR_BTN_ON);
		draw_shuffle(160.0f, 220.0f, shuffled ? CLR_GREEN : CLR_FAINT);

		/* Scrubber */
		C2D_DrawRectSolid(BAR_X, BAR_Y, 0.0f, BAR_W, 5.0f, CLR_TRACK);
		if (duration > 0) {
			float f = (float)progress / (float)duration;
			if (f < 0.0f)
				f = 0.0f;
			if (f > 1.0f)
				f = 1.0f;

			C2D_DrawRectSolid(BAR_X, BAR_Y, 0.0f, BAR_W * f, 5.0f, CLR_GREEN);

			/* Handle grows while dragging, for feedback under a thumb. */
			const float r = (g_scrub == SCRUB_DRAGGING) ? 10.0f : 6.0f;
			C2D_DrawRectSolid(BAR_X + BAR_W * f - r / 2.0f,
			                  BAR_Y + 2.5f - r / 2.0f, 0.0f, r, r, CLR_TEXT);

			fmt_time(progress, t_elapsed, sizeof t_elapsed);
			fmt_time(duration, t_total, sizeof t_total);
			draw_text_fit(textbuf, t_elapsed, BAR_X, BAR_Y + 12.0f, 0.38f,
			              80.0f, CLR_DIM);
			draw_text_fit(textbuf, t_total, BAR_X + BAR_W - 38.0f,
			              BAR_Y + 12.0f, 0.38f, 60.0f, CLR_DIM);
		}

		C3D_FrameEnd(0);

		/* The headless harness needs the app to exit on its own; a real console
		 * must not. So auto-exit is opt-in, enabled only by the presence of
		 * sdmc:/spotify/.smoketest (which dev.sh creates). */
		if (++frames == 420 && g_smoketest) {
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
