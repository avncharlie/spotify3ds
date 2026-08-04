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
#include "ui/screen_list.h"
#include "ui/screen_player.h"
#include "ui/screen_top.h"
#include "ui/thumbs.h"
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

/* Which view the bottom screen is showing. */
typedef enum { VIEW_PLAYER = 0, VIEW_LIST } bottom_view;
static bottom_view g_view;
static float       g_list_scroll;
static float       g_list_velocity;
static int         g_list_armed = -1;
static u64         g_list_arm_until;

/* List momentum is measured in pixels per frame. Keep it deliberately short:
 * this is a 240px resistive screen, so a phone-style multi-screen fling would
 * make the rows harder rather than easier to control. */
#define LIST_FLING_MAX      40.0f
#define LIST_FLING_FRICTION 0.88f
#define LIST_FLING_STOP     0.10f
#define LIST_ARM_MS         4000

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

/* Scratch for the worker's list snapshots.
 *
 * File scope rather than locals because these are large - recent_list is ~10KB
 * and playlist_list ~32KB - and several of the call sites run every frame. A
 * stack copy per frame is both a needless memcpy and, stacked with a TLS
 * handshake on the worker, enough to overflow. The render thread is the only
 * reader, so a single shared buffer is safe. */
static recent_list   g_recents_buf;
static playlist_list g_playlists_buf;
static album_list    g_albums_buf;

static int list_id_at(int pos, int recent_count, int playlist_count,
                      int album_count)
{
	if (pos < 0 || pos >= recent_count + playlist_count + album_count)
		return -1;
	if (pos < recent_count)
		return LIST_RECENT0 + pos;
	pos -= recent_count;
	if (pos < playlist_count)
		return LIST_PLAYLIST0 + pos;
	return LIST_ALBUM0 + pos - playlist_count;
}

static int list_move_id(int current, int direction, int recent_count,
                        int playlist_count, int album_count)
{
	const int total = recent_count + playlist_count + album_count;
	if (total <= 0)
		return -1;

	int pos = -1;
	if (current >= LIST_RECENT0 && current < LIST_RECENT0 + recent_count)
		pos = current - LIST_RECENT0;
	else if (current >= LIST_PLAYLIST0 &&
	         current < LIST_PLAYLIST0 + playlist_count)
		pos = recent_count + current - LIST_PLAYLIST0;
	else if (current >= LIST_ALBUM0 && current < LIST_ALBUM0 + album_count)
		pos = recent_count + playlist_count + current - LIST_ALBUM0;

	/* Either direction starts at the first row when nothing is selected. */
	if (pos < 0)
		return list_id_at(0, recent_count, playlist_count, album_count);

	pos += direction;
	if (pos < 0)
		pos = 0;
	if (pos >= total)
		pos = total - 1;
	return list_id_at(pos, recent_count, playlist_count, album_count);
}

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
	hidSetRepeatParameters(18, 5); /* 300ms delay, then about 12 rows/second */

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
	bool logged_recents     = false;
	bool logged_playlists   = false;
	bool logged_albums      = false;
	u64  repeat_probe_at    = 0;
	repeat_mode repeat_probe_from = REPEAT_OFF;

	while (aptMainLoop()) {
		hidScanInput();
		const u32 keys_down   = hidKeysDown();
		const u32 keys_repeat = hidKeysDownRepeat();
		if (keys_down & KEY_START)
			break;
		/* Y hides the cover. The top screen has no touch digitizer, so the
		 * art-off layout needs a physical button. */
		if (keys_down & KEY_Y)
			g_art_hidden = !g_art_hidden;

		/* Exercise the art-hidden layout headlessly too, so 2A cannot rot
		 * unnoticed: flip it for a stretch in the middle of a smoketest. */
		if (g_smoketest) {
			g_art_hidden = (frames > 480 && frames < 600);
			/* And the list view, so its draw path runs in every automated run
			 * rather than only when someone taps ALL by hand. */
			if (frames == 300)
				g_view = VIEW_LIST;
			/* Exercise the armed-row draw path during every automated run. */
			if (frames == 340) {
				g_list_armed = LIST_PLAYLIST0;
				g_list_arm_until = osGetTime() + 1000;
			}
			if (frames == 390)
				g_list_armed = -1;
			if (frames == 420) {
				tl_step("list_view", 1, "rendered %d frames", 120);
				g_view = VIEW_PLAYER;
				g_list_armed = -1;
			}
		}

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
		if (g_view == VIEW_PLAYER) {
			if (keys_down & KEY_A) {
				opt_set(&g_opt_play, !playing);
				worker_post(playing ? CMD_PAUSE : CMD_PLAY, 0);
			}
			if (keys_down & KEY_DRIGHT) {
				g_cmd_sent = osGetTime();
				tl_timing("button NEXT at %llu",
				          (unsigned long long)g_cmd_sent);
				worker_post(CMD_NEXT, 0);
			}
			if (keys_down & KEY_DLEFT) {
				g_cmd_sent = osGetTime();
				tl_timing("button PREV at %llu",
				          (unsigned long long)g_cmd_sent);
				worker_post(CMD_PREV, 0);
			}
		}

		if (g_view == VIEW_PLAYER && touch.pressed &&
		    touch.press_id == BTN_SCRUB && duration > 0)
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

		/* --- list view input ------------------------------------------- */
		if (g_view == VIEW_LIST) {
			recent_list *const   rl = &g_recents_buf;
			playlist_list *const pl = &g_playlists_buf;
			album_list *const    al = &g_albums_buf;
			const int            n  = worker_get_recents(rl);
			const int            pn = worker_get_playlists(pl);
			const int            an = worker_get_albums(al);

			if (g_list_armed >= 0 && osGetTime() >= g_list_arm_until)
				g_list_armed = -1;

			const u32 nav = keys_repeat & (KEY_UP | KEY_DOWN);
			if (nav) {
				const int direction = (nav & KEY_UP) ? -1 : 1;
				int next = -1;
				if (g_list_armed < 0)
					next = screen_list_section_first_id(
					    n, pn, an, g_list_scroll);
				if (next < 0)
					next = list_move_id(g_list_armed, direction, n, pn, an);
				g_list_armed = next;
				if (g_list_armed >= 0) {
					g_list_arm_until = osGetTime() + LIST_ARM_MS;
					g_list_velocity = 0.0f;
					const int buffer_id = list_move_id(
					    g_list_armed, direction, n, pn, an);
					g_list_scroll = screen_list_reveal_row(
					    n, pn, an, buffer_id, g_list_armed, g_list_scroll);
				}
			}

			if (keys_down & (KEY_L | KEY_R)) {
				const int direction = (keys_down & KEY_L) ? -1 : 1;
				g_list_armed = -1;
				g_list_velocity = 0.0f;
				g_list_scroll = screen_list_jump_section(
				    n, pn, an, g_list_scroll, direction);
			}

			/* Drag 1:1 while held, then retain a filtered portion of the final
			 * motion and decay it after release. A fresh touch always catches the
			 * list immediately. */
			if (touch.pressed)
				g_list_velocity = 0.0f;

			if (touch.down && touch.dragging) {
				/* Dragging is an unambiguous cancellation of any pending play. */
				g_list_armed = -1;
				const float delta = -(float)touch.dy;
				g_list_scroll += delta;
				/* Weight the newest sample heavily so release speed determines the
				 * fling: a slow lift coasts a few pixels, a fast flick travels farther. */
				g_list_velocity = g_list_velocity * 0.25f + delta * 0.75f;
				if (g_list_velocity > LIST_FLING_MAX)
					g_list_velocity = LIST_FLING_MAX;
				if (g_list_velocity < -LIST_FLING_MAX)
					g_list_velocity = -LIST_FLING_MAX;
			} else if (!touch.down) {
				g_list_scroll += g_list_velocity;
				g_list_velocity *= LIST_FLING_FRICTION;
				if (g_list_velocity > -LIST_FLING_STOP &&
				    g_list_velocity < LIST_FLING_STOP)
					g_list_velocity = 0.0f;
			}

			const float maxs = screen_list_max_scroll(n, pn, an, g_list_armed);
			if (g_list_scroll < 0.0f) {
				g_list_scroll = 0.0f;
				g_list_velocity = 0.0f;
			}
			if (g_list_scroll > maxs) {
				g_list_scroll = maxs;
				g_list_velocity = 0.0f;
			}

			if (touch.clicked == LIST_BTN_BACK || (keys_down & KEY_B)) {
				g_view = VIEW_PLAYER;
				g_list_armed = -1;
			} else if (touch.clicked == LIST_ARM_PLAY || (keys_down & KEY_A)) {
				const collection_item *item = NULL;
				if (g_list_armed >= LIST_RECENT0 &&
				    g_list_armed < LIST_RECENT0 + n)
					item = &rl->items[g_list_armed - LIST_RECENT0];
				else if (g_list_armed >= LIST_PLAYLIST0 &&
				         g_list_armed < LIST_PLAYLIST0 + pn)
					item = &pl->items[g_list_armed - LIST_PLAYLIST0];
				else if (g_list_armed >= LIST_ALBUM0 &&
				         g_list_armed < LIST_ALBUM0 + an)
					item = &al->items[g_list_armed - LIST_ALBUM0];

				if (item) {
					tl_log("list: confirmed play %s", item->context_uri);
					worker_play_context(item->context_uri);
					g_view = VIEW_PLAYER;
					opt_set(&g_opt_play, 1);
				}
				g_list_armed = -1;
			} else if (touch.clicked >= LIST_RECENT0 &&
			           touch.clicked < LIST_RECENT0 + RECENTS_MAX) {
				const int idx = touch.clicked - LIST_RECENT0;
				if (idx < n) {
					if (g_list_armed == touch.clicked) {
						g_list_armed = -1;
					} else {
						g_list_armed = touch.clicked;
						g_list_arm_until = osGetTime() + LIST_ARM_MS;
					}
				}
			} else if (touch.clicked >= LIST_PLAYLIST0 &&
			           touch.clicked < LIST_PLAYLIST0 + PLAYLISTS_MAX) {
				const int idx = touch.clicked - LIST_PLAYLIST0;
				if (idx < pn) {
					if (g_list_armed == touch.clicked) {
						g_list_armed = -1;
					} else {
						g_list_armed = touch.clicked;
						g_list_arm_until = osGetTime() + LIST_ARM_MS;
					}
				}
			} else if (touch.clicked >= LIST_ALBUM0 &&
			           touch.clicked < LIST_ALBUM0 + ALBUMS_MAX) {
				const int idx = touch.clicked - LIST_ALBUM0;
				if (idx < an) {
					if (g_list_armed == touch.clicked) {
						g_list_armed = -1;
					} else {
						g_list_armed = touch.clicked;
						g_list_arm_until = osGetTime() + LIST_ARM_MS;
					}
				}
			}
		}

		if (g_view == VIEW_PLAYER && touch.clicked == BTN_SHELF_ALL) {
			g_view        = VIEW_LIST;
			g_list_scroll = 0.0f;
			g_list_velocity = 0.0f;
			g_list_armed = -1;
		}

		if (g_view == VIEW_PLAYER && touch.clicked >= BTN_SHELF0 &&
		    touch.clicked < BTN_SHELF0 + SHELF_TILES) {
			recent_list *const rl  = &g_recents_buf;
			const int          n   = worker_get_recents(rl);
			const int          idx = touch.clicked - BTN_SHELF0;
			if (idx < n) {
				tl_log("shelf: play %s", rl->items[idx].context_uri);
				worker_play_context(rl->items[idx].context_uri);
				opt_set(&g_opt_play, 1);
			}
		}

		if (g_view == VIEW_PLAYER && touch.clicked >= 0 &&
		    touch.clicked != BTN_SCRUB) {
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

		/* Same for thumbnails, which have their own queue behind the hero. */
		thumbs_pump();

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
					                      art.tex_dim, art.accent_r,
					                      art.accent_g, art.accent_b, art.url,
					                      aerr, sizeof aerr);
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

		/* Phase 12: prove the shelf data arrives, and say what it is. A silent
		 * empty list is the failure this step exists to make impossible. */
		if (!logged_recents) {
			recent_list *const rl = &g_recents_buf;
			if (worker_get_recents(rl) > 0) {
				logged_recents = true;
				tl_step("recents", rl->count > 0, "%d items: %s | %s", rl->count,
				        rl->items[0].name,
				        rl->count > 1 ? rl->items[1].name : "-");
				for (int i = 0; i < rl->count && i < 4; i++)
					tl_log("  recent[%d] %s / %s -> %s", i, rl->items[i].name,
					       rl->items[i].subtitle, rl->items[i].context_uri);
			} else if (frames > 600) {
				logged_recents = true;
				tl_step("recents", 0, "no items after %d frames", frames);
			}
		}

		/* Phase 14: same for the playlist library. This is the section that
		 * actually fills the Library screen - the history dedupes to only a
		 * handful of collections - so an empty list here is the difference
		 * between a working screen and a blank one. */
		if (!logged_playlists) {
			playlist_list *const pl = &g_playlists_buf;

			if (worker_get_playlists(pl) > 0) {
				logged_playlists = true;
				tl_step("playlists", pl->count > 0, "%d of %d total: %s | %s",
				        pl->count, pl->total, pl->items[0].name,
				        pl->count > 1 ? pl->items[1].name : "-");

				int no_art = 0;
				for (int i = 0; i < pl->count; i++)
					if (!pl->items[i].art_url[0])
						no_art++;
				tl_log("  playlists without art: %d of %d", no_art, pl->count);

				for (int i = 0; i < pl->count && i < 3; i++)
					tl_log("  playlist[%d] %s / %s", i, pl->items[i].name,
					       pl->items[i].subtitle);
			} else if (frames > 650) {
				/* Must land before the 700-frame smoketest exit, or a genuine
				 * failure would never be reported at all. */
				logged_playlists = true;
				tl_step("playlists", 0, "no items after %d frames", frames);
			}
		}

		if (!logged_albums) {
			album_list *const al = &g_albums_buf;
			if (worker_get_albums(al) > 0) {
				logged_albums = true;
				tl_step("albums", al->count > 0, "%d of %d total: %s | %s",
				        al->count, al->total, al->items[0].name,
				        al->count > 1 ? al->items[1].name : "-");
				for (int i = 0; i < al->count && i < 3; i++)
					tl_log("  album[%d] %s / %s", i, al->items[i].name,
					       al->items[i].subtitle);
			} else if (frames > 650) {
				logged_albums = true;
				tl_step("albums", 0, "no items after %d frames", frames);
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

		if (g_view == VIEW_LIST) {
			recent_list *const   rl = &g_recents_buf;
			playlist_list *const pl = &g_playlists_buf;
			album_list *const    al = &g_albums_buf;
			worker_get_recents(rl);
			worker_get_playlists(pl);
			worker_get_albums(al);

			const screen_list_args la = {
				.buf        = textbuf,
				.tb         = &g_tb,
				.recents    = rl,
				.playlists  = pl,
				.albums     = al,
				.scroll     = g_list_scroll,
				.pressed_id = touch.down ? touch.press_id : -1,
				.armed_id   = g_list_armed,
			};
			screen_list_draw(&la);
		} else {
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

			/* Asking every frame is the intended use: a hit is a short scan and
			 * a miss queues the fetch once. */
			recent_list *const rl = &g_recents_buf;
			const int          rn = worker_get_recents(rl);
			for (int i = 0; i < SHELF_TILES && i < rn; i++)
				pa.shelf[i] = thumbs_get(rl->items[i].art_url);

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
	thumbs_free_all();
	net_exit();
	C2D_TextBufDelete(textbuf);
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return 0;
}
