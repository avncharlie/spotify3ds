#include <3ds.h>
#include <3ds/3dslink.h>
#include <citro2d.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "net/net.h"
#include "spotify/art.h"
#include "spotify/artcache.h"
#include "spotify/auth.h"
#include "spotify/player.h"
#include "testlog.h"
#include "ui/screen_list.h"
#include "ui/screen_lyrics.h"
#include "ui/screen_player.h"
#include "ui/screen_tracks.h"
#include "ui/screen_top.h"
#include "ui/thumbs.h"
#include "ui/touch.h"
#include "ui/ui.h"
#include "ui/volume_overlay.h"
#include "spotify/searchcache.h"
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
#define HOLD_SCRUB_DELAY_MS (TOUCH_TAP_TIMEOUT_MS + 1)
#define HOLD_SCRUB_STEP_MS  10000
#define HOLD_SCRUB_REPEAT_MS 1000

typedef struct {
	int  direction;
	bool scrubbing;
	u64  pressed_at;
	u64  next_seek_at;
	char track_uri[128];
} hold_scrub_state;

static hold_scrub_state g_dpad_scrub;
static hold_scrub_state g_touch_button_scrub;

/* Local clock for interpolating progress between polls, so the bar moves
 * smoothly at 60fps rather than jumping every 3s. */
static long g_base_progress;
static u64  g_base_time;

static album_art g_art;

/* KEY_Y hides the cover and switches the top screen to the large-title
 * layout. The top screen has no digitizer, so this has to be a button. */
static bool g_art_hidden;

/* Which view the bottom screen is showing. */
typedef enum {
	VIEW_PLAYER = 0,
	VIEW_LIST,
	VIEW_TRACKS,
	VIEW_LYRICS
} bottom_view;
static bottom_view g_view;
static bottom_view          g_lyrics_return_view = VIEW_PLAYER;
static worker_lyrics_status g_lyrics_status;
static worker_lyrics_payload g_lyrics_payload;
static lyrics_layout        g_lyrics_layout;
static char                 g_lyrics_requested_uri[128];
static float                g_lyrics_scroll;
static float                g_lyrics_velocity;
static bool                 g_lyrics_follow;
static bool                 g_lyrics_layout_error;
static bottom_view g_tracks_return_view = VIEW_LIST;
static float       g_list_scroll;
static float       g_list_velocity;
static int         g_list_armed = -1;
static u64         g_list_arm_until;

static collection_item       g_tracks_collection;
static worker_tracks_snapshot g_tracks_buf;
static float                 g_tracks_scroll;
static float                 g_tracks_velocity;
static int                   g_tracks_armed = -1;
static int                   g_tracks_cursor = -1;
static u64                   g_tracks_arm_until;
static unsigned              g_tracks_applied_generation;
static bool                  g_track_search_mode;
static char                  g_track_search_query[TRACK_SEARCH_QUERY_MAX + 1];
static worker_track_search_status g_track_search_status;
static worker_track_search_payload g_track_search_payload;
static track_page            g_track_search_page;
static unsigned              g_track_search_applied_generation;
/* Mirrors the Tracks input gate for the smoketest, which otherwise cannot tell
 * whether on-screen rows actually accept play/queue taps. */
static bool                  g_tracks_input_ready;
/* -2: leave unselected, -1: select last row, otherwise page-local index. */
static int                   g_tracks_select_on_load = -2;

/* List momentum is measured in pixels per frame. Keep it deliberately short:
 * this is a 240px resistive screen, so a phone-style multi-screen fling would
 * make the rows harder rather than easier to control. */
#define LIST_FLING_MAX      40.0f
#define LIST_FLING_FRICTION 0.88f
#define LIST_FLING_STOP     0.10f
#define LYRICS_DPAD_SPEED   5.0f
#define LYRICS_CPAD_SPEED   7.5f
#define LYRICS_CPAD_DEADZONE 20
#define LYRICS_CPAD_MAX     156.0f
#define LIST_ARM_MS         4000
#define TEXTBUF_GLYPHS      4096
#define VOLUME_STEP         5
#define VOLUME_OVERLAY_MS   1100
#define VOLUME_OPT_MS       12000
#define REPEAT_WRAP_MIN_MS  5000
#define REPEAT_WRAP_MAX_MS  10000

static void lyrics_drop_local(void)
{
	lyrics_layout_free(&g_lyrics_layout);
	worker_lyrics_payload_free(&g_lyrics_payload);
	g_lyrics_layout_error = false;
}

static void lyrics_request_current(const worker_snapshot *snap)
{
	lyrics_drop_local();
	g_lyrics_scroll = 0.0f;
	g_lyrics_velocity = 0.0f;
	g_lyrics_follow = true;
	memset(&g_lyrics_status, 0, sizeof g_lyrics_status);

	if (!snap || !snap->have_state || !snap->state.track_uri[0] ||
	    !snap->state.track[0] || !snap->state.artist[0]) {
		worker_cancel_lyrics();
		g_lyrics_requested_uri[0] = '\0';
		return;
	}

	snprintf(g_lyrics_requested_uri, sizeof g_lyrics_requested_uri, "%s",
	         snap->state.track_uri);
	worker_request_lyrics(snap->state.track_uri, snap->state.track,
	                      snap->state.artist, snap->state.album,
	                      snap->state.duration_ms);
	worker_get_lyrics_status(&g_lyrics_status);
}

static void lyrics_open_current(const worker_snapshot *snap)
{
	if (g_view == VIEW_LYRICS)
		return;
	g_lyrics_return_view = g_view;
	g_view = VIEW_LYRICS;
	g_scrub = SCRUB_IDLE;
	lyrics_request_current(snap);
}

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
	if (g_scrub == SCRUB_DRAGGING)
		return g_scrub_ms;

	if (!snap->have_state)
		return 0;

	long p = g_scrub == SCRUB_COMMITTING ? g_scrub_ms : g_base_progress;
	if (snap->state.is_playing)
		p += (long)(osGetTime() - g_base_time);

	if (snap->state.duration_ms > 0 && p > snap->state.duration_ms)
		p = snap->state.duration_ms;
	return p;
}

static void hold_scrub_start(hold_scrub_state *hold, int direction,
	                         const worker_snapshot *snap)
{
	memset(hold, 0, sizeof *hold);
	hold->direction = direction;
	hold->pressed_at = osGetTime();
	if (snap->have_state)
		snprintf(hold->track_uri, sizeof hold->track_uri, "%s",
		         snap->state.track_uri);
}

static void hold_scrub_cancel(hold_scrub_state *hold)
{
	memset(hold, 0, sizeof *hold);
}

static void hold_scrub_update(hold_scrub_state *hold, long progress_ms,
	                          long duration_ms,
	                          const worker_snapshot *snap)
{
	if (!hold->direction)
		return;
	if (hold->track_uri[0] &&
	    (!snap->have_state ||
	     strcmp(hold->track_uri, snap->state.track_uri) != 0)) {
		hold_scrub_cancel(hold);
		return;
	}
	if (!snap->have_state || !hold->track_uri[0] || duration_ms <= 0)
		return;

	const u64 now = osGetTime();
	if (!hold->scrubbing) {
		if (now - hold->pressed_at < HOLD_SCRUB_DELAY_MS)
			return;
		hold->scrubbing = true;
		hold->next_seek_at = now;
	}
	if (now < hold->next_seek_at)
		return;

	long target = progress_ms + hold->direction * HOLD_SCRUB_STEP_MS;
	if (target < 0)
		target = 0;
	if (target > duration_ms)
		target = duration_ms;
	if (target != progress_ms && worker_seek_track(target, hold->track_uri)) {
		g_scrub_ms = target;
		g_scrub = SCRUB_COMMITTING;
		g_scrub_until = now + SCRUB_COMMIT_MS;
		g_base_progress = target;
		g_base_time = now;
	}
	hold->next_seek_at = now + HOLD_SCRUB_REPEAT_MS;
}

/* Returns true when a hold became a scrub. A short press is left to the
 * caller's ordinary previous/next action. */
static bool hold_scrub_finish(hold_scrub_state *hold)
{
	const bool scrubbed = hold->scrubbing;
	memset(hold, 0, sizeof *hold);
	return scrubbed;
}

static void post_skip(int direction)
{
	g_cmd_sent = osGetTime();
	tl_timing("button %s at %llu", direction > 0 ? "NEXT" : "PREV",
	          (unsigned long long)g_cmd_sent);
	worker_post(direction > 0 ? CMD_NEXT : CMD_PREV, 0);
}


/* Somewhat clunky way to detect a track repeating.
 * returns true when current position moved backwards and
 * new posiiton is near track start and previous position was near track end */
static bool progress_wrapped(long previous_ms, long current_ms,
	                         long duration_ms)
{
	if (previous_ms < 0 || current_ms < 0 || duration_ms <= 0 ||
	    current_ms >= previous_ms)
		return false;
	long edge_ms = duration_ms / 4;
	if (edge_ms < REPEAT_WRAP_MIN_MS)
		edge_ms = REPEAT_WRAP_MIN_MS;
	if (edge_ms > REPEAT_WRAP_MAX_MS)
		edge_ms = REPEAT_WRAP_MAX_MS;
	return previous_ms >= duration_ms - edge_ms && current_ms <= edge_ms;
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

static opt_field g_opt_play, g_opt_shuf, g_opt_rep, g_opt_volume;
static char      g_opt_volume_device[128];
static char      g_seen_device[128];
static u64       g_volume_overlay_until;

typedef struct {
	char uri[128];
	u64  until;
} opt_target;

static opt_target g_opt_context, g_opt_track;

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
static recent_list   g_search_recents;
static playlist_list g_search_playlists;
static album_list    g_search_albums;
static char          g_list_search[64];
static char          g_filter_query[64];
static int           g_filter_playlist_count = -1;
static int           g_filter_album_count = -1;

static bool contains_ci(const char *text, const char *needle)
{
	if (!needle[0])
		return true;
	for (const char *p = text; *p; p++) {
		int i = 0;
		while (needle[i] && p[i] &&
		       tolower((unsigned char)p[i]) ==
		           tolower((unsigned char)needle[i]))
			i++;
		if (!needle[i])
			return true;
	}
	return false;
}

static bool collection_matches(const collection_item *item)
{
	return contains_ci(item->name, g_list_search) ||
	       contains_ci(item->subtitle, g_list_search);
}

static void library_get_lists(recent_list **recents, playlist_list **playlists,
	                          album_list **albums)
{
	worker_get_recents(&g_recents_buf);
	worker_get_playlists(&g_playlists_buf);
	worker_get_albums(&g_albums_buf);
	if (!g_list_search[0]) {
		*recents = &g_recents_buf;
		*playlists = &g_playlists_buf;
		*albums = &g_albums_buf;
		return;
	}

	if (strcmp(g_filter_query, g_list_search) != 0 ||
	    g_filter_playlist_count != g_playlists_buf.count ||
	    g_filter_album_count != g_albums_buf.count) {
		memset(&g_search_recents, 0, sizeof g_search_recents);
		memset(&g_search_playlists, 0, sizeof g_search_playlists);
		memset(&g_search_albums, 0, sizeof g_search_albums);
		for (int i = 0; i < g_playlists_buf.count; i++)
			if (collection_matches(&g_playlists_buf.items[i]))
				g_search_playlists.items[g_search_playlists.count++] =
				    g_playlists_buf.items[i];
		for (int i = 0; i < g_albums_buf.count; i++)
			if (collection_matches(&g_albums_buf.items[i]))
				g_search_albums.items[g_search_albums.count++] =
				    g_albums_buf.items[i];
		g_search_playlists.total = g_search_playlists.count;
		g_search_albums.total = g_search_albums.count;
		snprintf(g_filter_query, sizeof g_filter_query, "%s", g_list_search);
		g_filter_playlist_count = g_playlists_buf.count;
		g_filter_album_count = g_albums_buf.count;
	}

	*recents = &g_search_recents;
	*playlists = &g_search_playlists;
	*albums = &g_search_albums;
}

static void library_reset_position(void)
{
	g_list_scroll = 0.0f;
	g_list_velocity = 0.0f;
	g_list_armed = -1;
}

static void library_edit_search(void)
{
	SwkbdState keyboard;
	char query[sizeof g_list_search];
	snprintf(query, sizeof query, "%s", g_list_search);
	swkbdInit(&keyboard, SWKBD_TYPE_NORMAL, 2, (int)sizeof query - 1);
	swkbdSetHintText(&keyboard, "Albums and playlists");
	swkbdSetInitialText(&keyboard, query);
	swkbdSetButton(&keyboard, SWKBD_BUTTON_LEFT, "Cancel", false);
	swkbdSetButton(&keyboard, SWKBD_BUTTON_RIGHT, "Find", true);
	if (swkbdInputText(&keyboard, query, sizeof query) != SWKBD_BUTTON_RIGHT)
		return;

	char *start = query;
	while (*start && isspace((unsigned char)*start))
		start++;
	char *end = start + strlen(start);
	while (end > start && isspace((unsigned char)end[-1]))
		*--end = '\0';
	snprintf(g_list_search, sizeof g_list_search, "%s", start);
	g_filter_query[0] = '\0';
	library_reset_position();
}

static const collection_item *list_selected_item(int id, const recent_list *rl,
	                                             const playlist_list *pl,
	                                             const album_list *al)
{
	if (id >= LIST_RECENT0 && id < LIST_RECENT0 + rl->count)
		return &rl->items[id - LIST_RECENT0];
	if (id >= LIST_PLAYLIST0 && id < LIST_PLAYLIST0 + pl->count)
		return &pl->items[id - LIST_PLAYLIST0];
	if (id >= LIST_ALBUM0 && id < LIST_ALBUM0 + al->count)
		return &al->items[id - LIST_ALBUM0];
	return NULL;
}

static const collection_item *list_chevron_item(int id, const recent_list *rl,
	                                            const playlist_list *pl,
	                                            const album_list *al)
{
	if (id >= LIST_CHEVRON_RECENT0 &&
	    id < LIST_CHEVRON_RECENT0 + rl->count)
		return &rl->items[id - LIST_CHEVRON_RECENT0];
	if (id >= LIST_CHEVRON_PLAYLIST0 &&
	    id < LIST_CHEVRON_PLAYLIST0 + pl->count)
		return &pl->items[id - LIST_CHEVRON_PLAYLIST0];
	if (id >= LIST_CHEVRON_ALBUM0 &&
	    id < LIST_CHEVRON_ALBUM0 + al->count)
		return &al->items[id - LIST_CHEVRON_ALBUM0];
	return NULL;
}

static const collection_item *list_play_item(int id, const recent_list *rl,
	                                         const playlist_list *pl,
	                                         const album_list *al)
{
	if (id >= LIST_PLAY_RECENT0 && id < LIST_PLAY_RECENT0 + rl->count)
		return &rl->items[id - LIST_PLAY_RECENT0];
	if (id >= LIST_PLAY_PLAYLIST0 &&
	    id < LIST_PLAY_PLAYLIST0 + pl->count)
		return &pl->items[id - LIST_PLAY_PLAYLIST0];
	if (id >= LIST_PLAY_ALBUM0 && id < LIST_PLAY_ALBUM0 + al->count)
		return &al->items[id - LIST_PLAY_ALBUM0];
	return NULL;
}

static void tracks_request_page(int offset, int select_on_load)
{
	g_tracks_scroll = 0.0f;
	g_tracks_velocity = 0.0f;
	g_tracks_armed = -1;
	g_tracks_cursor = -1;
	g_tracks_select_on_load = select_on_load;
	worker_request_tracks(&g_tracks_collection, offset);
}

static void tracks_search_reset_position(void)
{
	g_tracks_scroll = 0.0f;
	g_tracks_velocity = 0.0f;
	g_tracks_armed = -1;
	g_tracks_cursor = -1;
}

/* Refresh the visible page from a snapshot that changed underneath it - either
 * a scan finding more matches, or a rescan replacing results served from a
 * stale corpus, which can just as easily remove them. The user may already be
 * reading the rows, so the scroll offset and cursor stay put; only a cursor or
 * scroll left past the end is pulled back. */
static void tracks_search_refresh_page(void)
{
	const int previous = g_track_search_page.count;
	track_search_build_page(&g_track_search_payload.results,
	                        &g_tracks_collection, g_track_search_page.offset,
	                        &g_track_search_page);
	if (g_track_search_page.count == previous)
		return;
	if (g_tracks_cursor >= TRACK_ROW0 &&
	    g_tracks_cursor - TRACK_ROW0 >= g_track_search_page.count) {
		g_tracks_cursor = g_track_search_page.count > 0
		                      ? TRACK_ROW0 + g_track_search_page.count - 1
		                      : -1;
		if (g_tracks_armed >= TRACK_ROW0)
			g_tracks_armed = g_tracks_cursor;
	}
	const float max = screen_tracks_max_scroll(g_track_search_page.count,
	                                           g_tracks_armed);
	if (g_tracks_scroll > max)
		g_tracks_scroll = max < 0.0f ? 0.0f : max;
}

static void tracks_search_build_page(int offset, int select_on_load)
{
	offset = offset < 0 ? 0 : (offset / TRACK_PAGE_MAX) * TRACK_PAGE_MAX;
	track_search_build_page(&g_track_search_payload.results,
	                        &g_tracks_collection, offset,
	                        &g_track_search_page);
	tracks_search_reset_position();
	g_tracks_select_on_load = select_on_load;
	if (g_track_search_page.count > 0 && select_on_load != -2) {
		int index = select_on_load < 0 ? g_track_search_page.count - 1
		                                   : select_on_load;
		if (index >= g_track_search_page.count)
			index = g_track_search_page.count - 1;
		g_tracks_armed = TRACK_ROW0 + index;
		g_tracks_cursor = g_tracks_armed;
		g_tracks_arm_until = osGetTime() + LIST_ARM_MS;
		g_tracks_scroll = screen_tracks_reveal_row(
		    g_track_search_page.count, g_tracks_armed, g_tracks_armed,
		    g_tracks_scroll);
	}
}

static void tracks_clear_search(void)
{
	worker_cancel_track_search();
	worker_track_search_payload_free(&g_track_search_payload);
	memset(&g_track_search_status, 0, sizeof g_track_search_status);
	memset(&g_track_search_page, 0, sizeof g_track_search_page);
	g_track_search_mode = false;
	g_track_search_query[0] = '\0';
	g_track_search_applied_generation = 0;
	tracks_search_reset_position();
}

static void tracks_edit_search(void)
{
	SwkbdState keyboard;
	char query[sizeof g_track_search_query];
	snprintf(query, sizeof query, "%s", g_track_search_query);
	swkbdInit(&keyboard, SWKBD_TYPE_NORMAL, 2, (int)sizeof query - 1);
	swkbdSetHintText(&keyboard, "Track, artist, or album");
	swkbdSetInitialText(&keyboard, query);
	swkbdSetButton(&keyboard, SWKBD_BUTTON_LEFT, "Cancel", false);
	swkbdSetButton(&keyboard, SWKBD_BUTTON_RIGHT, "Find", true);
	if (swkbdInputText(&keyboard, query, sizeof query) != SWKBD_BUTTON_RIGHT)
		return;
	char *start = query;
	while (*start && isspace((unsigned char)*start))
		start++;
	char *end = start + strlen(start);
	while (end > start && isspace((unsigned char)end[-1]))
		*--end = '\0';
	if (!start[0]) {
		tracks_clear_search();
		return;
	}
	worker_track_search_payload_free(&g_track_search_payload);
	memset(&g_track_search_status, 0, sizeof g_track_search_status);
	memset(&g_track_search_page, 0, sizeof g_track_search_page);
	snprintf(g_track_search_query, sizeof g_track_search_query, "%s", start);
	g_track_search_mode = true;
	g_track_search_applied_generation = 0;
	tracks_search_reset_position();
	worker_request_track_search(&g_tracks_collection, g_track_search_query);
	worker_get_track_search_status(&g_track_search_status);
}

static void tracks_open(const collection_item *item)
{
	if (!item)
		return;
	g_tracks_return_view = g_view == VIEW_PLAYER ? VIEW_PLAYER : VIEW_LIST;
	tracks_clear_search();
	g_tracks_collection = *item;
	g_view = VIEW_TRACKS;
	g_tracks_applied_generation = 0;
	tracks_request_page(0, -2);
}

static bool collection_named(const char *name, collection_item *out)
{
	for (int i = 0; i < g_playlists_buf.count; i++) {
		if (strcasecmp(g_playlists_buf.items[i].name, name) == 0) {
			*out = g_playlists_buf.items[i];
			return true;
		}
	}
	for (int i = 0; i < g_recents_buf.count; i++) {
		if (strcasecmp(g_recents_buf.items[i].name, name) == 0) {
			*out = g_recents_buf.items[i];
			return true;
		}
	}
	return false;
}

static bool track_page_offsets_valid(const track_page *page)
{
	if (!page || page->count < 0 || page->count > TRACK_PAGE_MAX ||
	    page->offset < 0 || page->total < page->offset + page->count)
		return false;
	for (int i = 0; i < page->count; i++)
		if (page->items[i].source_index != page->offset + i)
			return false;
	return true;
}

static bool recent_contexts_unique(const recent_list *list)
{
	for (int i = 0; i < list->count; i++)
		for (int j = i + 1; j < list->count; j++)
			if (strcmp(list->items[i].context_uri,
			           list->items[j].context_uri) == 0)
				return false;
	return true;
}

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

static void opt_set_for(opt_field *o, long v, u64 duration_ms)
{
	o->value = v;
	o->until = osGetTime() + duration_ms;
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

static int effective_volume(const worker_snapshot *snap)
{
	if (!snap->have_state || !snap->state.volume_known)
		return 0;
	if (strcmp(g_opt_volume_device, snap->state.device_id) == 0)
		return (int)opt_get(&g_opt_volume, snap->state.volume_percent);
	return (int)snap->state.volume_percent;
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

static const char *current_collection_uri(const player_state *state)
{
	if (strncmp(state->context_uri, "spotify:playlist:", 17) == 0 ||
	    strncmp(state->context_uri, "spotify:album:", 14) == 0)
		return state->context_uri;
	return strncmp(state->album_uri, "spotify:album:", 14) == 0
	           ? state->album_uri
	           : "";
}

static bool target_matches(const opt_target *target, const char *uri)
{
	return uri && uri[0] && osGetTime() < target->until &&
	       strcmp(target->uri, uri) == 0;
}

static void target_set(opt_target *target, const char *uri)
{
	snprintf(target->uri, sizeof target->uri, "%s", uri);
	target->until = osGetTime() + OPTIMISTIC_MS;
}

static void activate_collection(const collection_item *item,
	                            const worker_snapshot *snap, bool playing)
{
	const bool current =
	    (snap->have_state &&
	     strcmp(item->context_uri, current_collection_uri(&snap->state)) == 0) ||
	    target_matches(&g_opt_context, item->context_uri);
	if (current) {
		tl_log("list: %s current %s", playing ? "pause" : "resume",
		       item->context_uri);
		opt_set(&g_opt_play, !playing);
		worker_post(playing ? CMD_PAUSE : CMD_PLAY, 0);
	} else {
		tl_log("list: play %s", item->context_uri);
		worker_play_context(item->context_uri);
		target_set(&g_opt_context, item->context_uri);
		opt_set(&g_opt_play, 1);
	}
}

static void activate_track(const track_item *item,
	                       const worker_snapshot *snap, bool playing)
{
	const bool current =
	    (snap->have_state && strcmp(item->uri, snap->state.track_uri) == 0) ||
	    target_matches(&g_opt_track, item->uri);
	if (current) {
		tl_log("track: %s current %s", playing ? "pause" : "resume",
		       item->uri);
		opt_set(&g_opt_play, !playing);
		worker_post(playing ? CMD_PAUSE : CMD_PLAY, 0);
	} else {
		tl_log("track: play context=%s item=%s position=%d name=%s",
		       g_tracks_collection.context_uri, item->uri, item->source_index,
		       item->name);
		if (worker_play_context_item(g_tracks_collection.context_uri, item->uri)) {
			target_set(&g_opt_track, item->uri);
			opt_set(&g_opt_play, 1);
		}
	}
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
	artcache_init();

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

	C2D_TextBuf textbuf = C2D_TextBufNew(TEXTBUF_GLYPHS);
	worker_lyrics_payload_init(&g_lyrics_payload);
	worker_track_search_payload_init(&g_track_search_payload);

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
		tl_step("worker_start", 1, "network thread started");
	}

	touch_state     touch = {.press_id = -1, .clicked = -1};
	worker_snapshot snap  = {0};
	char            last_art[256] = "";

	long last_seen_progress = -1;
	char last_seen_track_uri[128] = "";
	int  frames             = 0;
	size_t max_text_glyphs  = 0;
	bool logged_first       = false;
	bool logged_recents     = false;
	bool logged_playlists   = false;
	bool logged_albums      = false;
	bool volume_overlay_supported_drawn = false;
	bool volume_overlay_zero_drawn = false;
	bool volume_overlay_unsupported_drawn = false;
	u64  repeat_probe_at    = 0;
	repeat_mode repeat_probe_from = REPEAT_OFF;
	int      tracks_probe_stage = 0;
	unsigned tracks_probe_generation = 0;
	int      tracks_probe_far_offset = 0;
	u64      tracks_probe_play_at = 0;
	unsigned tracks_probe_poll_seq = 0;
	char     tracks_probe_expected[128] = "";
	char     tracks_probe_item_uri[128] = "";
	collection_item tracks_probe_good = {0};
	collection_item tracks_probe_lux = {0};
	bool     search_probe_saw_partial = false;
	bool     search_probe_input_live = false;
	int      search_probe_partial_count = 0;
	int      search_probe_partial_scanned = 0;
	int      search_probe_partial_total = 0;
	int      cache_probe_first_count = 0;
	int      cache_probe_first_scanned = 0;
	u64      cache_probe_started = 0;

	while (aptMainLoop()) {
		hidScanInput();
		const u32 keys_down   = hidKeysDown();
		const u32 keys_held   = hidKeysHeld();
		const u32 keys_up     = hidKeysUp();
		const u32 keys_repeat = hidKeysDownRepeat();
		/* Deliberate two-button exit chord. Require one half to transition this
		 * frame so holding both cannot retrigger unrelated input before shutdown. */
		if ((keys_held & (KEY_L | KEY_START)) == (KEY_L | KEY_START) &&
		    (keys_down & (KEY_L | KEY_START)))
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
			if (frames == 360) {
				snprintf(g_list_search, sizeof g_list_search, "tame");
				g_filter_query[0] = '\0';
				library_reset_position();
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
		if (snap.have_state &&
		    strcmp(g_seen_device, snap.state.device_id) != 0) {
			snprintf(g_seen_device, sizeof g_seen_device, "%s",
			         snap.state.device_id);
			g_opt_volume.until = 0;
			g_opt_volume_device[0] = '\0';
		}
		if (snap.have_state && g_opt_volume.until && snap.state.volume_known &&
		    strcmp(g_opt_volume_device, snap.state.device_id) == 0 &&
		    snap.state.volume_percent == g_opt_volume.value) {
			g_opt_volume.until = 0;
			g_opt_volume_device[0] = '\0';
		}

		/* Re-base on either progress or URI. Two consecutive tracks can report the
		 * same numerical position, which must not preserve the previous clock. */
		if (snap.have_state &&
		    (snap.state.progress_ms != last_seen_progress ||
		     strcmp(snap.state.track_uri, last_seen_track_uri) != 0)) {
			const bool track_changed =
			    strcmp(snap.state.track_uri, last_seen_track_uri) != 0;
			const bool repeated =
			    !track_changed && g_scrub != SCRUB_COMMITTING &&
			    progress_wrapped(last_seen_progress, snap.state.progress_ms,
			                     snap.state.duration_ms);
			last_seen_progress = snap.state.progress_ms;
			snprintf(last_seen_track_uri, sizeof last_seen_track_uri, "%s",
			         snap.state.track_uri);
			g_base_progress    = snap.state.progress_ms;
			g_base_time        = osGetTime();
			if (track_changed)
				g_scrub = SCRUB_IDLE;
			if (repeated && g_view == VIEW_LYRICS && g_lyrics_follow) {
				g_lyrics_scroll = 0.0f;
				g_lyrics_velocity = 0.0f;
			}

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
		/* START replaces the old app-exit shortcut and opens lyrics from any
		 * ordinary view. Capture the return view before changing input dispatch. */
		if (g_view != VIEW_LYRICS && (keys_down & KEY_START)) {
			lyrics_open_current(&snap);
		}
		const bottom_view input_view = g_view;
		const u32 dpad_down = keys_down & (KEY_DLEFT | KEY_DRIGHT);
		const bool player_bar_drag = input_view == VIEW_PLAYER && touch.down &&
		                             touch.press_id == BTN_SCRUB;
		if (player_bar_drag)
			hold_scrub_cancel(&g_dpad_scrub);
		if (!player_bar_drag && !g_touch_button_scrub.direction &&
		    !g_dpad_scrub.direction &&
		    dpad_down != (KEY_DLEFT | KEY_DRIGHT)) {
			if (dpad_down & KEY_DRIGHT)
				hold_scrub_start(&g_dpad_scrub, 1, &snap);
			else if (dpad_down & KEY_DLEFT)
				hold_scrub_start(&g_dpad_scrub, -1, &snap);
		}
		if (g_dpad_scrub.direction) {
			const u32 held_key = g_dpad_scrub.direction > 0 ? KEY_DRIGHT
			                                                  : KEY_DLEFT;
			if (keys_held & held_key) {
				hold_scrub_update(&g_dpad_scrub, progress, duration, &snap);
			} else if ((keys_up & held_key) || !(keys_held & held_key)) {
				const int direction = g_dpad_scrub.direction;
				if (!hold_scrub_finish(&g_dpad_scrub))
					post_skip(direction);
			}
		}

		if (input_view == VIEW_PLAYER && touch.pressed &&
		    (touch.press_id == BTN_PREV || touch.press_id == BTN_NEXT)) {
			hold_scrub_cancel(&g_dpad_scrub);
			hold_scrub_start(&g_touch_button_scrub,
			                 touch.press_id == BTN_NEXT ? 1 : -1, &snap);
		}
		if (g_touch_button_scrub.direction) {
			if (input_view != VIEW_PLAYER || touch.dragging) {
				hold_scrub_cancel(&g_touch_button_scrub);
			} else if (touch.down) {
				hold_scrub_update(&g_touch_button_scrub, progress, duration, &snap);
			} else if (touch.released) {
				const int direction = g_touch_button_scrub.direction;
				if (!hold_scrub_finish(&g_touch_button_scrub) &&
				    touch.clicked < 0)
					post_skip(direction);
			}
		}

		if (input_view == VIEW_LYRICS) {
			if (!snap.have_state || !snap.state.track_uri[0]) {
				if (g_lyrics_requested_uri[0] || g_lyrics_payload.doc.count)
					lyrics_request_current(NULL);
			} else if (strcmp(g_lyrics_requested_uri,
			                  snap.state.track_uri) != 0) {
				lyrics_request_current(&snap);
			}

			worker_get_lyrics_status(&g_lyrics_status);
			worker_lyrics_payload incoming;
			worker_lyrics_payload_init(&incoming);
			if (worker_take_lyrics(&incoming)) {
				const bool current = snap.have_state &&
				                     strcmp(incoming.track_uri,
				                            snap.state.track_uri) == 0 &&
				                     incoming.generation == g_lyrics_status.generation;
				if (current && lyrics_layout_build(&g_lyrics_layout, textbuf,
				                                   &incoming.doc)) {
					worker_lyrics_payload_move(&g_lyrics_payload, &incoming);
					g_lyrics_layout_error = false;
				} else if (current) {
					g_lyrics_layout_error = true;
				}
			}
			worker_lyrics_payload_free(&incoming);
		}

		const bool lyrics_document_ready =
		    input_view == VIEW_LYRICS && snap.have_state &&
		    g_lyrics_payload.doc.count > 0 &&
		    strcmp(g_lyrics_payload.track_uri, snap.state.track_uri) == 0 &&
		    g_lyrics_layout.count == g_lyrics_payload.doc.count;
		const int lyrics_highlight =
		    lyrics_document_ready && g_lyrics_payload.doc.synced
		        ? lyrics_index_at(&g_lyrics_payload.doc,
		                          progress > 0 ? (uint32_t)progress : 0)
		        : -1;

		const u32 volume_keys = keys_repeat & (KEY_L | KEY_R);
		if (volume_keys) {
			g_volume_overlay_until = osGetTime() + VOLUME_OVERLAY_MS;
			const bool supported = snap.have_state &&
			                       snap.state.supports_volume &&
			                       snap.state.volume_known &&
			                       snap.state.device_id[0];
			if (supported && volume_keys != (KEY_L | KEY_R)) {
				const int current = effective_volume(&snap);
				int target = current + ((volume_keys & KEY_R) ? VOLUME_STEP
				                                              : -VOLUME_STEP);
				if (target < 0)
					target = 0;
				if (target > 100)
					target = 100;
				if (target != current &&
				    worker_set_volume(target, snap.state.device_id)) {
					opt_set_for(&g_opt_volume, target, VOLUME_OPT_MS);
					snprintf(g_opt_volume_device,
					         sizeof g_opt_volume_device, "%s",
					         snap.state.device_id);
				}
			}
		}

		if (input_view == VIEW_TRACKS) {
			worker_get_tracks(&g_tracks_buf);
			if (g_track_search_mode) {
				worker_get_track_search_status(&g_track_search_status);
				worker_track_search_payload incoming;
				worker_track_search_payload_init(&incoming);
				if (worker_take_track_search(&incoming)) {
					const bool current =
					    incoming.generation == g_track_search_status.generation &&
					    strcmp(incoming.context_uri,
					           g_tracks_collection.context_uri) == 0 &&
					    strcmp(incoming.query, g_track_search_query) == 0;
					if (current) {
						const bool first =
						    g_track_search_applied_generation !=
						    incoming.generation;
						worker_track_search_payload_move(&g_track_search_payload,
						                                 &incoming);
						g_track_search_applied_generation =
						    g_track_search_payload.generation;
						/* The first snapshot builds the page from scratch;
						 * later ones only extend what the user is already
						 * looking at. */
						if (first)
							tracks_search_build_page(0, -2);
						else
							tracks_search_refresh_page();
					}
				}
				worker_track_search_payload_free(&incoming);
			} else if (g_tracks_buf.state == TRACKS_READY &&
			    g_tracks_buf.generation != g_tracks_applied_generation) {
				g_tracks_applied_generation = g_tracks_buf.generation;
				g_tracks_collection = g_tracks_buf.page.collection;
				g_tracks_scroll = 0.0f;
				g_tracks_velocity = 0.0f;
				g_tracks_armed = -1;
				g_tracks_cursor = -1;
				if (g_tracks_buf.page.count > 0 &&
				    g_tracks_select_on_load != -2) {
					int idx = g_tracks_select_on_load < 0
					              ? g_tracks_buf.page.count - 1
					              : g_tracks_select_on_load;
					if (idx >= g_tracks_buf.page.count)
						idx = g_tracks_buf.page.count - 1;
					g_tracks_armed = TRACK_ROW0 + idx;
					g_tracks_cursor = g_tracks_armed;
					g_tracks_arm_until = osGetTime() + LIST_ARM_MS;
					int buffer_idx = idx;
					if (g_tracks_select_on_load < 0 && idx > 0)
						buffer_idx--;
					else if (g_tracks_select_on_load == 0 &&
					         idx + 1 < g_tracks_buf.page.count)
						buffer_idx++;
					g_tracks_scroll = screen_tracks_reveal_row(
					    g_tracks_buf.page.count, g_tracks_armed, g_tracks_armed,
					    g_tracks_scroll);
					g_tracks_scroll = screen_tracks_reveal_row(
					    g_tracks_buf.page.count, TRACK_ROW0 + buffer_idx,
					    g_tracks_armed, g_tracks_scroll);
				}
				g_tracks_select_on_load = -2;

			}
		}

		/* --- input ---------------------------------------------------- */
		if (input_view == VIEW_LYRICS) {
			if (keys_down & KEY_SELECT) {
				opt_set(&g_opt_play, !playing);
				worker_post(playing ? CMD_PAUSE : CMD_PLAY, 0);
			}
			if ((keys_down & KEY_B) || touch.clicked == LYRICS_BTN_BACK) {
				worker_cancel_lyrics();
				lyrics_drop_local();
				g_lyrics_requested_uri[0] = '\0';
				g_view = g_lyrics_return_view;
			} else {
				const bool retry = (keys_down & KEY_X) ||
				                   touch.clicked == LYRICS_BTN_RETRY;
				if (retry && snap.have_state)
					lyrics_request_current(&snap);

				if (touch.clicked == LYRICS_BTN_FOLLOW &&
				    lyrics_document_ready && g_lyrics_payload.doc.synced)
					g_lyrics_follow = true;

				if (lyrics_document_ready && g_lyrics_payload.doc.synced &&
				    touch.clicked >= LYRICS_LINE0) {
					const int line = touch.clicked - LYRICS_LINE0;
					if (line >= 0 &&
					    (size_t)line < g_lyrics_payload.doc.count) {
						long target =
						    (long)g_lyrics_payload.doc.lines[line].time_ms;
						if (duration > 0 && target > duration)
							target = duration;
						if (worker_seek_track(target, snap.state.track_uri)) {
							g_scrub_ms = target;
							g_scrub = SCRUB_COMMITTING;
							g_scrub_until = osGetTime() + SCRUB_COMMIT_MS;
							g_base_progress = target;
							g_base_time = osGetTime();
							g_lyrics_follow = true;
						}
					}
				}

				float nav_velocity = 0.0f;
				const u32 lyric_nav = keys_held & (KEY_UP | KEY_DOWN);
				if (lyric_nav != (KEY_UP | KEY_DOWN)) {
					if (lyric_nav & KEY_UP)
						nav_velocity -= LYRICS_DPAD_SPEED;
					if (lyric_nav & KEY_DOWN)
						nav_velocity += LYRICS_DPAD_SPEED;
				}
				circlePosition circle;
				hidCircleRead(&circle);
				const int circle_y = circle.dy;
				const int circle_magnitude = abs(circle_y);
				if (circle_magnitude > LYRICS_CPAD_DEADZONE) {
					const float strength =
					    (float)(circle_magnitude - LYRICS_CPAD_DEADZONE) /
					    (LYRICS_CPAD_MAX - LYRICS_CPAD_DEADZONE);
					nav_velocity += (circle_y > 0 ? -1.0f : 1.0f) *
					                strength * LYRICS_CPAD_SPEED;
				}
				if (nav_velocity > LYRICS_CPAD_SPEED)
					nav_velocity = LYRICS_CPAD_SPEED;
				if (nav_velocity < -LYRICS_CPAD_SPEED)
					nav_velocity = -LYRICS_CPAD_SPEED;

				const bool content_touch = touch.start_py >= 30 &&
				                           touch.start_py < 236;
				if (touch.pressed && content_touch)
					g_lyrics_velocity = 0.0f;
				if (touch.down && touch.dragging && content_touch) {
					const float delta = -(float)touch.dy;
					g_lyrics_scroll += delta;
					g_lyrics_velocity =
					    g_lyrics_velocity * 0.25f + delta * 0.75f;
					if (g_lyrics_velocity > LIST_FLING_MAX)
						g_lyrics_velocity = LIST_FLING_MAX;
					if (g_lyrics_velocity < -LIST_FLING_MAX)
						g_lyrics_velocity = -LIST_FLING_MAX;
					g_lyrics_follow = false;
				} else if (!touch.down && lyrics_document_ready &&
				           (nav_velocity < -0.01f || nav_velocity > 0.01f)) {
					g_lyrics_velocity +=
					    (nav_velocity - g_lyrics_velocity) * 0.24f;
					g_lyrics_scroll += g_lyrics_velocity;
					g_lyrics_follow = false;
				} else if (!touch.down && !g_lyrics_follow) {
					g_lyrics_scroll += g_lyrics_velocity;
					g_lyrics_velocity *= LIST_FLING_FRICTION;
					if (g_lyrics_velocity > -LIST_FLING_STOP &&
					    g_lyrics_velocity < LIST_FLING_STOP)
						g_lyrics_velocity = 0.0f;
				}

				const screen_lyrics_args metrics = {
					.buf = textbuf,
					.doc = lyrics_document_ready ? &g_lyrics_payload.doc : NULL,
					.layout = lyrics_document_ready ? &g_lyrics_layout : NULL,
					.highlight = lyrics_highlight,
					.scroll = g_lyrics_scroll,
				};
				const float max_scroll = screen_lyrics_max_scroll(&metrics);
				if (g_lyrics_follow && lyrics_document_ready &&
				    g_lyrics_payload.doc.synced) {
					const float target = screen_lyrics_follow_scroll(&metrics);
					g_lyrics_scroll += (target - g_lyrics_scroll) * 0.15f;
					if (g_lyrics_scroll > target - 0.25f &&
					    g_lyrics_scroll < target + 0.25f)
						g_lyrics_scroll = target;
					g_lyrics_velocity = 0.0f;
				}
				if (g_lyrics_scroll < 0.0f) {
					g_lyrics_scroll = 0.0f;
					g_lyrics_velocity = 0.0f;
				}
				if (g_lyrics_scroll > max_scroll) {
					g_lyrics_scroll = max_scroll;
					g_lyrics_velocity = 0.0f;
				}
			}
		}
		if (input_view == VIEW_PLAYER) {
			if (keys_down & KEY_A) {
				opt_set(&g_opt_play, !playing);
				worker_post(playing ? CMD_PAUSE : CMD_PLAY, 0);
			}
		}
		if (input_view == VIEW_LIST || input_view == VIEW_TRACKS) {
			if (keys_down & KEY_SELECT) {
				opt_set(&g_opt_play, !playing);
				worker_post(playing ? CMD_PAUSE : CMD_PLAY, 0);
			}
		}

		if (input_view == VIEW_PLAYER && touch.pressed &&
		    touch.press_id == BTN_SCRUB && duration > 0)
			g_scrub = SCRUB_DRAGGING;

		if (input_view == VIEW_PLAYER && !g_touch_button_scrub.direction &&
		    g_scrub == SCRUB_DRAGGING &&
		    touch.down && duration > 0) {
			float f = ((float)touch.px - SCRUB_BAR_X) / SCRUB_BAR_W;
			if (f < 0.0f)
				f = 0.0f;
			if (f > 1.0f)
				f = 1.0f;
			g_scrub_ms = (long)(f * (float)duration);
		}

		if (input_view == VIEW_PLAYER && g_scrub == SCRUB_DRAGGING &&
		    touch.released) {
			worker_post(CMD_SEEK, g_scrub_ms);
			g_base_progress = g_scrub_ms;
			g_base_time = osGetTime();
			g_scrub       = SCRUB_COMMITTING;
			g_scrub_until = osGetTime() + SCRUB_COMMIT_MS;
		}

		/* --- list view input ------------------------------------------- */
		if (input_view == VIEW_LIST) {
			recent_list *rl;
			playlist_list *pl;
			album_list *al;
			library_get_lists(&rl, &pl, &al);
			const int n = rl->count;
			const int pn = pl->count;
			const int an = al->count;
			const bool filtering = g_list_search[0] != '\0';

			if (g_list_armed >= 0 && osGetTime() >= g_list_arm_until)
				g_list_armed = -1;

			const u32 nav = keys_repeat & (KEY_UP | KEY_DOWN);
			if (nav) {
				const int direction = (nav & KEY_UP) ? -1 : 1;
				int next = -1;
				if (g_list_armed < 0)
					next = screen_list_section_first_id(
					    n, pn, an, g_list_scroll, filtering);
				if (next < 0)
					next = list_move_id(g_list_armed, direction, n, pn, an);
				g_list_armed = next;
				if (g_list_armed >= 0) {
					g_list_arm_until = osGetTime() + LIST_ARM_MS;
					g_list_velocity = 0.0f;
					const int buffer_id = list_move_id(
					    g_list_armed, direction, n, pn, an);
					g_list_scroll = screen_list_reveal_row(
					    n, pn, an, buffer_id, g_list_armed, g_list_scroll,
					    filtering);
				}
			}

			if (keys_down & (KEY_ZL | KEY_ZR)) {
				const int direction = (keys_down & KEY_ZL) ? -1 : 1;
				g_list_armed = -1;
				g_list_velocity = 0.0f;
				g_list_scroll = screen_list_jump_section(
				    n, pn, an, g_list_scroll, direction, filtering);
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

			const float maxs =
			    screen_list_max_scroll(n, pn, an, g_list_armed, filtering);
			if (g_list_scroll < 0.0f) {
				g_list_scroll = 0.0f;
				g_list_velocity = 0.0f;
			}
			if (g_list_scroll > maxs) {
				g_list_scroll = maxs;
				g_list_velocity = 0.0f;
			}

			const collection_item *selected =
			    list_selected_item(g_list_armed, rl, pl, al);
			const collection_item *drilldown =
			    list_chevron_item(touch.clicked, rl, pl, al);
			const collection_item *direct_play =
			    list_play_item(touch.clicked, rl, pl, al);
			if (touch.clicked == LIST_BTN_FIND) {
				library_edit_search();
			} else if (touch.clicked == LIST_BTN_CLEAR_SEARCH) {
				g_list_search[0] = '\0';
				g_filter_query[0] = '\0';
				library_reset_position();
			} else if (direct_play) {
				activate_collection(direct_play, &snap, playing);
				g_list_armed = -1;
			} else if (drilldown) {
				tracks_open(drilldown);
			} else if ((keys_down & KEY_X) && selected) {
				tracks_open(selected);
			} else if (touch.clicked == LIST_BTN_BACK || (keys_down & KEY_B)) {
				g_view = VIEW_PLAYER;
				g_list_armed = -1;
			} else if (keys_down & KEY_A) {
				if (selected) {
					activate_collection(selected, &snap, playing);
					g_list_arm_until = osGetTime() + LIST_ARM_MS;
				}
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

		/* --- collection track input ------------------------------------ */
		if (input_view == VIEW_TRACKS) {
			track_page *const page = g_track_search_mode
			                               ? &g_track_search_page
			                               : &g_tracks_buf.page;
			/* Rows drawn from a partial snapshot must be playable straight
			 * away, so this matches what the screen shows rather than waiting
			 * for the scan to finish. */
			const bool ready =
			    g_track_search_mode
			        ? g_track_search_applied_generation != 0 &&
			              (g_track_search_status.state == TRACK_SEARCH_READY ||
			               g_track_search_page.count > 0)
			        : g_tracks_buf.state == TRACKS_READY;
			g_tracks_input_ready = ready;

			if (touch.clicked == TRACK_BTN_BACK || (keys_down & KEY_B)) {
				if (g_track_search_mode) {
					tracks_clear_search();
				} else {
					worker_cancel_tracks();
					g_view = g_tracks_return_view;
					g_tracks_armed = -1;
					g_tracks_cursor = -1;
				}
			} else if (touch.clicked == TRACK_BTN_SEARCH) {
				tracks_edit_search();
			} else if (touch.clicked == TRACK_BTN_CLEAR_SEARCH) {
				/* B also leaves the collection, so the X is the only
				 * unambiguous way back to the full track list. The buffered
				 * page is still there, so this is the same cheap return B
				 * makes rather than a refetch. */
				tracks_clear_search();
			} else if ((touch.clicked == TRACK_BTN_RETRY ||
			            (keys_down & KEY_X)) &&
			           (g_track_search_mode
			                ? g_track_search_status.state == TRACK_SEARCH_ERROR
			                : g_tracks_buf.state == TRACKS_ERROR)) {
				if (g_track_search_mode) {
					worker_track_search_payload_free(&g_track_search_payload);
					g_track_search_applied_generation = 0;
					worker_request_track_search(&g_tracks_collection,
					                            g_track_search_query);
				} else {
					tracks_request_page(page->offset, -2);
				}
			} else if (ready) {
				if (g_tracks_armed >= 0 && osGetTime() >= g_tracks_arm_until)
					g_tracks_armed = -1;

				const bool prev_page =
				    touch.clicked == TRACK_BTN_PREV_PAGE || (keys_down & KEY_ZL);
				const bool next_page =
				    touch.clicked == TRACK_BTN_NEXT_PAGE || (keys_down & KEY_ZR);
				if (g_track_search_mode && prev_page && page->offset > 0) {
					tracks_search_build_page(page->offset - TRACK_PAGE_MAX, -2);
				} else if (g_track_search_mode && prev_page &&
				           page->collection.kind == COLLECTION_PLAYLIST &&
				           page->total > page->count) {
					const int last_offset =
					    ((page->total - 1) / TRACK_PAGE_MAX) * TRACK_PAGE_MAX;
					tracks_search_build_page(last_offset, -2);
				} else if (g_track_search_mode && next_page &&
				           page->offset + page->count < page->total) {
					tracks_search_build_page(page->offset + TRACK_PAGE_MAX, -2);
				} else if (g_track_search_mode && next_page &&
				           page->collection.kind == COLLECTION_PLAYLIST &&
				           page->offset > 0 && page->total > page->count) {
					tracks_search_build_page(0, -2);
				} else if (!g_track_search_mode && prev_page && page->offset > 0) {
					tracks_request_page(page->offset - TRACK_PAGE_MAX, -2);
				} else if (!g_track_search_mode && prev_page &&
				           page->collection.kind == COLLECTION_PLAYLIST &&
				           page->total > page->count) {
					const int last_offset =
					    ((page->total - 1) / TRACK_PAGE_MAX) * TRACK_PAGE_MAX;
					tracks_request_page(last_offset, -2);
				} else if (!g_track_search_mode && next_page &&
				           page->offset + page->count < page->total) {
					tracks_request_page(page->offset + TRACK_PAGE_MAX, -2);
				} else if (!g_track_search_mode && next_page &&
				           page->collection.kind == COLLECTION_PLAYLIST &&
				           page->offset > 0 && page->total > page->count) {
					tracks_request_page(0, -2);
				} else {
					const u32 nav = keys_repeat & (KEY_UP | KEY_DOWN);
					if (nav && page->count > 0) {
						const int direction = nav & KEY_UP ? -1 : 1;
						int idx = g_tracks_cursor >= TRACK_ROW0
						              ? g_tracks_cursor - TRACK_ROW0
						              : (direction < 0 ? page->count - 1 : 0);
						if (g_tracks_cursor >= TRACK_ROW0)
							idx += direction;

						if (g_track_search_mode && idx < 0 &&
						    page->offset > 0) {
							tracks_search_build_page(page->offset - TRACK_PAGE_MAX,
							                         -1);
						} else if (g_track_search_mode && idx < 0 &&
						           page->collection.kind == COLLECTION_PLAYLIST &&
						           page->total > page->count) {
							const int last_offset =
							    ((page->total - 1) / TRACK_PAGE_MAX) * TRACK_PAGE_MAX;
							tracks_search_build_page(last_offset, -1);
						} else if (g_track_search_mode && idx >= page->count &&
						           page->offset + page->count < page->total) {
							tracks_search_build_page(page->offset + TRACK_PAGE_MAX, 0);
						} else if (g_track_search_mode && idx >= page->count &&
						           page->collection.kind == COLLECTION_PLAYLIST &&
						           page->offset > 0 && page->total > page->count) {
							tracks_search_build_page(0, 0);
						} else if (!g_track_search_mode && idx < 0 && page->offset > 0) {
							tracks_request_page(page->offset - TRACK_PAGE_MAX, -1);
						} else if (!g_track_search_mode && idx < 0 &&
						           page->collection.kind == COLLECTION_PLAYLIST &&
						           page->total > page->count) {
							const int last_offset =
							    ((page->total - 1) / TRACK_PAGE_MAX) * TRACK_PAGE_MAX;
							tracks_request_page(last_offset, -1);
						} else if (!g_track_search_mode && idx >= page->count &&
						           page->offset + page->count < page->total) {
							tracks_request_page(page->offset + TRACK_PAGE_MAX, 0);
						} else if (!g_track_search_mode && idx >= page->count &&
						           page->collection.kind == COLLECTION_PLAYLIST &&
						           page->offset > 0 && page->total > page->count) {
							tracks_request_page(0, 0);
						} else {
							if (idx < 0)
								idx = 0;
							if (idx >= page->count)
								idx = page->count - 1;
							g_tracks_armed = TRACK_ROW0 + idx;
							g_tracks_cursor = g_tracks_armed;
							g_tracks_arm_until = osGetTime() + LIST_ARM_MS;
							g_tracks_velocity = 0.0f;
							int buffer_idx = idx + direction;
							if (buffer_idx < 0)
								buffer_idx = 0;
							if (buffer_idx >= page->count)
								buffer_idx = page->count - 1;
							g_tracks_scroll = screen_tracks_reveal_row(
							    page->count, g_tracks_armed, g_tracks_armed,
							    g_tracks_scroll);
							g_tracks_scroll = screen_tracks_reveal_row(
							    page->count, TRACK_ROW0 + buffer_idx,
							    g_tracks_armed, g_tracks_scroll);
						}
					}

					if (touch.pressed)
						g_tracks_velocity = 0.0f;
					if (touch.down && touch.dragging) {
						g_tracks_armed = -1;
						const float delta = -(float)touch.dy;
						g_tracks_scroll += delta;
						g_tracks_velocity =
						    g_tracks_velocity * 0.25f + delta * 0.75f;
						if (g_tracks_velocity > LIST_FLING_MAX)
							g_tracks_velocity = LIST_FLING_MAX;
						if (g_tracks_velocity < -LIST_FLING_MAX)
							g_tracks_velocity = -LIST_FLING_MAX;
					} else if (!touch.down) {
						g_tracks_scroll += g_tracks_velocity;
						g_tracks_velocity *= LIST_FLING_FRICTION;
						if (g_tracks_velocity > -LIST_FLING_STOP &&
						    g_tracks_velocity < LIST_FLING_STOP)
							g_tracks_velocity = 0.0f;
					}

					const float maxs =
					    screen_tracks_max_scroll(page->count, g_tracks_armed);
					if (g_tracks_scroll < 0) {
						g_tracks_scroll = 0;
						g_tracks_velocity = 0;
					}
					if (g_tracks_scroll > maxs) {
						g_tracks_scroll = maxs;
						g_tracks_velocity = 0;
					}

					const int idx = g_tracks_armed - TRACK_ROW0;
					const int play_idx = touch.clicked - TRACK_PLAY0;
					int queue_idx = touch.clicked - TRACK_QUEUE0;
					if ((queue_idx < 0 || queue_idx >= page->count) &&
					    (keys_down & KEY_X))
						queue_idx = idx;
					if (play_idx >= 0 && play_idx < page->count &&
					    page->items[play_idx].playable) {
						activate_track(&page->items[play_idx], &snap, playing);
						g_tracks_armed = -1;
					} else if (queue_idx >= 0 && queue_idx < page->count &&
					    page->items[queue_idx].playable) {
						tl_log("track: queue item=%s name=%s",
						       page->items[queue_idx].uri,
						       page->items[queue_idx].name);
						worker_queue_item(page->items[queue_idx].uri);
					} else if ((keys_down & KEY_A) &&
					    idx >= 0 && idx < page->count && page->items[idx].playable) {
						activate_track(&page->items[idx], &snap, playing);
						g_tracks_arm_until = osGetTime() + LIST_ARM_MS;
					} else if (touch.clicked >= TRACK_ROW0 &&
					           touch.clicked < TRACK_ROW0 + page->count) {
						g_tracks_cursor = touch.clicked;
						if (g_tracks_armed == touch.clicked)
							g_tracks_armed = -1;
						else {
							g_tracks_armed = touch.clicked;
							g_tracks_arm_until = osGetTime() + LIST_ARM_MS;
							g_tracks_scroll = screen_tracks_reveal_row(
							    page->count, g_tracks_armed, g_tracks_armed,
							    g_tracks_scroll);
						}
					}
				}
			}
		}

		if (input_view == VIEW_PLAYER &&
		    (touch.clicked == BTN_SHELF_ALL || (keys_down & KEY_X))) {
			g_view        = VIEW_LIST;
			g_list_scroll = 0.0f;
			g_list_velocity = 0.0f;
			g_list_armed = -1;
		}

		if (input_view == VIEW_PLAYER && touch.clicked >= BTN_SHELF0 &&
		    touch.clicked < BTN_SHELF0 + SHELF_TILES) {
			recent_list *const rl  = &g_recents_buf;
			const int          n   = worker_get_recents(rl);
			const int          idx = touch.clicked - BTN_SHELF0;
			if (idx < n) {
				tl_log("shelf: open %s", rl->items[idx].context_uri);
				tracks_open(&rl->items[idx]);
			}
		}

		if (input_view == VIEW_PLAYER && touch.long_pressed >= BTN_SHELF0 &&
		    touch.long_pressed < BTN_SHELF0 + SHELF_TILES) {
			recent_list *const rl = &g_recents_buf;
			const int n = worker_get_recents(rl);
			const int idx = touch.long_pressed - BTN_SHELF0;
			if (idx < n) {
				tl_log("shelf: long-play %s", rl->items[idx].context_uri);
				worker_play_context(rl->items[idx].context_uri);
				opt_set(&g_opt_play, 1);
			}
		}

		if (input_view == VIEW_PLAYER && touch.clicked >= 0 &&
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
				case BTN_LYRICS:
					lyrics_open_current(&snap);
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
				const char *current = snap.have_state
				                          ? current_collection_uri(&snap.state)
				                          : "";
				tl_step("recents_current",
				        recent_contexts_unique(rl) &&
				            (!current[0] ||
				             strcmp(rl->items[0].context_uri, current) == 0),
				        "current=%s first=%s unique=%d",
				        current[0] ? current : "-", rl->items[0].context_uri,
				        (int)recent_contexts_unique(rl));
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

		/* Album loading can finish after the fixed list-view rendering window.
		 * Keep the search fixture alive until both source lists can be filtered. */
		if (g_smoketest && frames >= 420 && g_list_search[0]) {
			recent_list *search_recents;
			playlist_list *search_playlists;
			album_list *search_albums;
			library_get_lists(&search_recents, &search_playlists, &search_albums);
			if (g_albums_buf.count > 0 || frames > 650) {
				tl_step("list_search",
				        g_albums_buf.count > 0 && search_recents->count == 0 &&
				            search_albums->count > 0,
				        "query=%s playlists=%d albums=%d", g_list_search,
				        search_playlists->count, search_albums->count);
				g_list_search[0] = '\0';
				g_filter_query[0] = '\0';
			}
		}

		/* Named live fixtures exercise the track browser against both extremes:
		 * Good music is intentionally enormous, while LUX picks is small enough
		 * to validate ordinary one-page use. Every transition goes through the
		 * same bounded worker snapshot as the interactive UI. */
		if (g_smoketest && frames > 430 && tracks_probe_stage < 99) {
			static worker_tracks_snapshot tracks;
			worker_get_tracks(&tracks);

			switch (tracks_probe_stage) {
				case 0:
					worker_get_playlists(&g_playlists_buf);
					worker_get_recents(&g_recents_buf);
					if (collection_named("good music", &tracks_probe_good) &&
					    collection_named("lux picks", &tracks_probe_lux)) {
						g_tracks_collection = tracks_probe_good;
						g_view = VIEW_TRACKS;
						tracks_probe_generation =
						    worker_request_tracks(&tracks_probe_good, 0);
						tracks_probe_stage = 1;
					} else if (frames > 900) {
						tl_step("tracks_fixtures", 0,
						        "missing good music or lux picks");
						tracks_probe_stage = 99;
					}
					break;

				case 1:
					if (tracks.generation != tracks_probe_generation ||
					    tracks.state == TRACKS_LOADING)
						break;
					if (tracks.state != TRACKS_READY) {
						tl_step("tracks_good_first", 0, "%s", tracks.error);
						tracks_probe_stage = 99;
						break;
					}
					tracks_probe_good = tracks.page.collection;
					tl_step("tracks_good_first",
					        tracks.page.total > TRACK_PAGE_MAX &&
					            tracks.page.count == TRACK_PAGE_MAX &&
					            track_page_offsets_valid(&tracks.page),
					        "count=%d total=%d", tracks.page.count,
					        tracks.page.total);
					tracks_probe_far_offset =
					    ((tracks.page.total - 1) / TRACK_PAGE_MAX) * TRACK_PAGE_MAX;
					tracks_probe_generation = worker_request_tracks(
					    &tracks_probe_good, tracks_probe_far_offset);
					tracks_probe_stage = 2;
					break;

				case 2:
					if (tracks.generation != tracks_probe_generation ||
					    tracks.state == TRACKS_LOADING)
						break;
					if (tracks.state != TRACKS_READY) {
						tl_step("tracks_good_far", 0, "%s", tracks.error);
						tracks_probe_stage = 99;
						break;
					}
					tl_step("tracks_good_far",
					        tracks.page.offset == tracks_probe_far_offset &&
					            tracks.page.count > 0 &&
					            tracks.page.offset + tracks.page.count ==
					                tracks.page.total &&
					            track_page_offsets_valid(&tracks.page),
					        "offset=%d count=%d total=%d", tracks.page.offset,
					        tracks.page.count, tracks.page.total);
					for (int i = 0; i < tracks.page.count; i++) {
						if (!tracks.page.items[i].playable)
							continue;
						snprintf(tracks_probe_expected,
						         sizeof tracks_probe_expected, "%s",
						         tracks.page.items[i].name);
						snprintf(tracks_probe_item_uri,
						         sizeof tracks_probe_item_uri, "%s",
						         tracks.page.items[i].uri);
						break;
					}
					tracks_probe_generation =
					    worker_request_tracks(&tracks_probe_good, 0);
					tracks_probe_stage = 3;
					break;

				case 3:
					if (tracks.generation != tracks_probe_generation ||
					    tracks.state == TRACKS_LOADING)
						break;
					if (tracks.state != TRACKS_READY) {
						tl_step("tracks_refetch", 0, "%s", tracks.error);
						tracks_probe_stage = 99;
						break;
					}
					tl_step("tracks_refetch",
					        tracks.page.offset == 0 && tracks.page.count > 0 &&
					            track_page_offsets_valid(&tracks.page),
					        "offset=%d count=%d", tracks.page.offset,
					        tracks.page.count);
					g_tracks_collection = tracks_probe_lux;
					tracks_probe_generation =
					    worker_request_tracks(&tracks_probe_lux, 0);
					tracks_probe_stage = 4;
					break;

				case 4:
					if (tracks.generation != tracks_probe_generation ||
					    tracks.state == TRACKS_LOADING)
						break;
					if (tracks.state != TRACKS_READY) {
						tl_step("tracks_lux", 0, "%s", tracks.error);
						tracks_probe_stage = 99;
						break;
					}
					tracks_probe_lux = tracks.page.collection;
					int art_count = 0;
					for (int i = 0; i < tracks.page.count; i++)
						if (tracks.page.items[i].art_url[0])
							art_count++;
					tl_step("tracks_lux",
					        tracks.page.count > 0 &&
					            tracks.page.total < tracks_probe_good.item_total &&
					            track_page_offsets_valid(&tracks.page) && art_count > 0,
					        "count=%d total=%d art=%d", tracks.page.count,
					        tracks.page.total, art_count);
					if (!snap.have_state) {
						tl_step("tracks_play", 1,
						        "skipped - no active Spotify device");
						tracks_probe_stage = 6;
						break;
					}
					if (worker_play_context_item(tracks_probe_good.context_uri,
					                             tracks_probe_item_uri)) {
						tracks_probe_play_at = osGetTime();
						tracks_probe_poll_seq = snap.poll_seq;
					}
					if (!tracks_probe_play_at) {
						tl_step("tracks_play", 0, "no playable final-page item");
						tracks_probe_stage = 6;
					} else {
						tracks_probe_stage = 5;
					}
					break;

				case 5: {
					const bool arrived = snap.poll_seq > tracks_probe_poll_seq &&
					                     snap.have_state &&
					                     strcmp(snap.state.track,
					                            tracks_probe_expected) == 0 &&
					                     strcmp(snap.state.track_uri,
					                            tracks_probe_item_uri) == 0 &&
					                     strcmp(snap.state.context_uri,
					                            tracks_probe_good.context_uri) == 0;
					const bool expired =
					    osGetTime() - tracks_probe_play_at > 15000;
					if (!arrived && !expired)
						break;
					tl_step("tracks_play", arrived || !snap.have_state,
					        arrived ? "wanted=%s got=%s"
					                : "skipped - active device disappeared",
					        tracks_probe_expected,
					        snap.have_state ? snap.state.track : "-");
					tracks_probe_stage = 6;
					break;
				}

				case 6:
					worker_get_albums(&g_albums_buf);
					if (g_albums_buf.count <= 0)
						break;
					g_tracks_collection = g_albums_buf.items[0];
					tracks_probe_generation = worker_request_tracks(
					    &g_tracks_collection, 0);
					tracks_probe_stage = 7;
					break;

				case 7:
					if (tracks.generation != tracks_probe_generation ||
					    tracks.state == TRACKS_LOADING)
						break;
					if (tracks.state != TRACKS_READY) {
						tl_step("tracks_album", 0, "%s", tracks.error);
					} else {
						tl_step("tracks_album",
						        tracks.page.count > 0 &&
						            track_page_offsets_valid(&tracks.page) &&
						            tracks.page.items[0].art_url[0],
						        "%s count=%d total=%d art=%s",
						        tracks.page.collection.name, tracks.page.count,
						        tracks.page.total,
						        tracks.page.items[0].art_url[0] ? "yes" : "no");
					}
					tl_step("tracks_view", 1, "bounded loading and page views rendered");
					/* Search the large playlist: streaming only has something
					 * to show when the scan spans many pages, so a one-page
					 * collection cannot exercise this path at all. */
					g_view = VIEW_TRACKS;
					tracks_clear_search();
					g_tracks_collection = tracks_probe_good;
					snprintf(g_track_search_query, sizeof g_track_search_query,
					         "%s", "a");
					g_track_search_mode = true;
					g_track_search_applied_generation = 0;
					tracks_probe_generation = worker_request_track_search(
					    &tracks_probe_good, g_track_search_query);
					tracks_probe_stage = 8;
					break;

				case 8: {
					/* Matches must become visible and playable while the scan
					 * is still running, not only once it finishes. */
					worker_track_search_status st;
					worker_get_track_search_status(&st);
					if (st.state == TRACK_SEARCH_ERROR) {
						tl_step("search_stream", 0, "%s", st.error);
						tracks_probe_stage = 9;
						break;
					}
					if (st.state == TRACK_SEARCH_LOADING &&
					    g_track_search_page.count > 0 &&
					    !search_probe_saw_partial) {
						search_probe_saw_partial = true;
						search_probe_partial_count = g_track_search_page.count;
						search_probe_partial_scanned = st.scanned;
						search_probe_partial_total = st.source_total;
						/* Read the gate the input block actually used: if it
						 * is false, play and queue taps are dead on screen. */
						search_probe_input_live = g_tracks_input_ready;
					}
					/* Reporting as soon as streaming is proven keeps the probe
					 * inside the frame budget; scanning ~1800 tracks to the end
					 * would take longer than the whole smoketest allows. */
					if (search_probe_saw_partial ||
					    st.state == TRACK_SEARCH_READY) {
						const bool partial_mid_scan =
						    search_probe_saw_partial &&
						    search_probe_partial_scanned <
						        search_probe_partial_total;
						tl_step("search_stream", partial_mid_scan,
						        "partial=%d rows=%d at %d/%d scanned",
						        (int)search_probe_saw_partial,
						        search_probe_partial_count,
						        search_probe_partial_scanned,
						        search_probe_partial_total);
						tl_step("search_stream_input", search_probe_input_live,
						        "rows interactive while scanning=%d",
						        (int)search_probe_input_live);
						tracks_probe_stage = 9;
					}
					break;
				}

				case 9:
					/* A small collection, so the scan can actually finish
					 * inside the frame budget: the retained corpus is only
					 * installed on a complete walk. */
					tracks_clear_search();
					g_tracks_collection = tracks_probe_lux;
					snprintf(g_track_search_query, sizeof g_track_search_query,
					         "%s", "a");
					g_track_search_mode = true;
					g_track_search_applied_generation = 0;
					worker_request_track_search(&tracks_probe_lux,
					                            g_track_search_query);
					tracks_probe_stage = 10;
					break;

				case 10: {
					worker_track_search_status st;
					worker_get_track_search_status(&st);
					if (st.state == TRACK_SEARCH_ERROR) {
						tl_step("search_cache_build", 0, "%s", st.error);
						tracks_probe_stage = 12;
						break;
					}
					if (st.state != TRACK_SEARCH_READY)
						break;
					cache_probe_first_count = st.matched_total;
					cache_probe_first_scanned = st.scanned;
					/* Either path is correct: a first launch walks the
					 * collection, a later one is answered by the stored
					 * corpus. What must hold is that the search completed
					 * and found the rows. */
					/* On a first launch this walks the collection; on a
					 * later one the stored corpus answers it. Either is
					 * correct, but a cache hit here must have come off the
					 * card, since nothing has been searched yet this run. */
					tl_step("search_cache_build",
					        cache_probe_first_count > 0 &&
					            cache_probe_first_scanned > 0 &&
					            (!st.from_cache || !st.from_memory),
					        "matched=%d scanned=%d from_disk=%d",
					        cache_probe_first_count, cache_probe_first_scanned,
					        (int)st.from_cache);
					/* Same collection, different query, with the stored copy
					 * removed so only the in-memory corpus can answer. */
					{
						char p2[160];
						const char *l2 =
						    strrchr(tracks_probe_lux.context_uri, ':');
						snprintf(p2, sizeof p2,
						         "sdmc:/spotify/searchidx/%s.s3i",
						         l2 ? l2 + 1 : "");
						remove(p2);
					}
					tracks_clear_search();
					g_tracks_collection = tracks_probe_lux;
					snprintf(g_track_search_query, sizeof g_track_search_query,
					         "%s", "e");
					g_track_search_mode = true;
					g_track_search_applied_generation = 0;
					cache_probe_started = osGetTime();
					worker_request_track_search(&tracks_probe_lux,
					                            g_track_search_query);
					tracks_probe_stage = 11;
					break;
				}

				case 11: {
					worker_track_search_status st;
					worker_get_track_search_status(&st);
					if (st.state == TRACK_SEARCH_ERROR) {
						tl_step("search_cache_hit", 0, "%s", st.error);
						tracks_probe_stage = 12;
						break;
					}
					if (st.state != TRACK_SEARCH_READY)
						break;
					const u64 took = osGetTime() - cache_probe_started;
					/* Deleting the stored entry first makes this specific to
					 * the in-memory corpus: with nothing on the card to fall
					 * back to, answering at all proves the retained one was
					 * used. Wall-clock is not the assertion - the worker ticks
					 * at 100ms and validation shares those ticks - but it is
					 * worth recording. */
					tl_step("search_cache_hit",
					        st.from_cache && st.from_memory,
					        "memory=%d cache=%d matched=%d in %ums",
					        (int)st.from_memory, (int)st.from_cache,
					        st.matched_total, (unsigned)took);
					tracks_probe_stage = 12;
					break;
				}

				case 12: {
					/* Damage the stored entry and confirm the store heals:
					 * a corrupt file must be discarded on read rather than
					 * trusted, and the search must still answer. The
					 * in-memory corpus is checked separately by
					 * search_cache_hit. */
					char idxpath[160];
					const char *lid =
					    strrchr(tracks_probe_lux.context_uri, ':');
					snprintf(idxpath, sizeof idxpath,
					         "sdmc:/spotify/searchidx/%s.s3i",
					         lid ? lid + 1 : "");
					FILE *f = fopen(idxpath, "r+b");
					if (!f) {
						tl_step("search_cache_stale", 0, "no stored index");
						tracks_probe_stage = 14;
						break;
					}
					/* The snapshot id sits at offset 8 in the header. */
					fseek(f, 8, SEEK_SET);
					fputc('!', f);
					fclose(f);

					tracks_clear_search();
					g_tracks_collection = tracks_probe_lux;
					snprintf(g_track_search_query, sizeof g_track_search_query,
					         "%s", "a");
					g_track_search_mode = true;
					g_track_search_applied_generation = 0;
					worker_request_track_search(&tracks_probe_lux,
					                            g_track_search_query);
					tracks_probe_stage = 13;
					break;
				}

				case 13: {
					worker_track_search_status st;
					worker_get_track_search_status(&st);
					if (st.state == TRACK_SEARCH_ERROR) {
						tl_step("search_cache_stale", 0, "%s", st.error);
						tracks_probe_stage = 14;
						break;
					}
					if (st.state != TRACK_SEARCH_READY)
						break;
					/* The search above is answered from memory, which is the
					 * point: a damaged file on the card must never be able to
					 * break it. What remains is proving the damaged bytes are
					 * rejected rather than parsed, which is a property of the
					 * loader and is asserted directly. */
					char idxpath[160];
					const char *lid =
					    strrchr(tracks_probe_lux.context_uri, ':');
					snprintf(idxpath, sizeof idxpath,
					         "sdmc:/spotify/searchidx/%s.s3i",
					         lid ? lid + 1 : "");
					const bool rejected =
					    searchcache_load(tracks_probe_lux.context_uri) == NULL;
					const bool removed = fopen(idxpath, "rb") == NULL;
					tl_step("search_cache_stale",
					        rejected && st.matched_total > 0,
					        "answered=%d corrupt rejected=%d deleted=%d",
					        st.matched_total, (int)rejected, (int)removed);
					tracks_probe_stage = 14;
					break;
				}

				case 14: {
					/* Deleting entries by hand must read as a miss, never as
					 * an error: the store is disposable by design. */
					char idxpath[160];
					const char *lid =
					    strrchr(tracks_probe_lux.context_uri, ':');
					snprintf(idxpath, sizeof idxpath,
					         "sdmc:/spotify/searchidx/%s.s3i",
					         lid ? lid + 1 : "");
					remove(idxpath);
					remove("sdmc:/spotify/searchidx/index.txt");
					tracks_clear_search();
					g_tracks_collection = tracks_probe_lux;
					snprintf(g_track_search_query, sizeof g_track_search_query,
					         "%s", "a");
					g_track_search_mode = true;
					g_track_search_applied_generation = 0;
					worker_request_track_search(&tracks_probe_lux,
					                            g_track_search_query);
					tracks_probe_stage = 15;
					break;
				}

				case 15: {
					worker_track_search_status st;
					worker_get_track_search_status(&st);
					if (st.state == TRACK_SEARCH_ERROR) {
						tl_step("search_cache_deletable", 0, "%s", st.error);
						tracks_probe_stage = 16;
						break;
					}
					if (st.state != TRACK_SEARCH_READY)
						break;
					tl_step("search_cache_deletable", st.matched_total > 0,
					        "recovered after deletion matched=%d",
					        st.matched_total);

					/* The rebuild must have gone back to the card. Reading the
					 * file directly is the only way to prove the store works:
					 * every other assertion here is also satisfied by a search
					 * that simply never cached anything. */
					char idxpath[160];
					const char *lid =
					    strrchr(tracks_probe_lux.context_uri, ':');
					snprintf(idxpath, sizeof idxpath,
					         "sdmc:/spotify/searchidx/%s.s3i",
					         lid ? lid + 1 : "");
					long stored = 0;
					FILE *w = fopen(idxpath, "rb");
					if (w) {
						fseek(w, 0, SEEK_END);
						stored = ftell(w);
						fclose(w);
					}
					tl_step("search_cache_persisted", stored > 0,
					        "%s is %ld bytes after rebuild", idxpath, stored);
					tracks_probe_stage = 16;
					break;
				}

				case 16:
					tracks_clear_search();
					g_view = VIEW_PLAYER;
					tracks_probe_stage = 99;
					break;
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
			tl_step("volume_state",
			        snap.state.device_id[0] &&
			            (!snap.state.supports_volume ||
			             (snap.state.volume_known &&
			              snap.state.volume_percent >= 0 &&
			              snap.state.volume_percent <= 100)),
			        "known=%d volume=%ld supported=%d",
			        (int)snap.state.volume_known, snap.state.volume_percent,
			        (int)snap.state.supports_volume);
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
		const bool lyrics_view = g_view == VIEW_LYRICS;
		const bool lyrics_loading =
		    lyrics_view && snap.have_state &&
		    g_lyrics_status.state == WORKER_LYRICS_LOADING &&
		    strcmp(g_lyrics_status.track_uri, snap.state.track_uri) == 0;
		const bool lyrics_error =
		    lyrics_view &&
		    (g_lyrics_layout_error ||
		     (g_lyrics_status.state == WORKER_LYRICS_ERROR && snap.have_state &&
		      strcmp(g_lyrics_status.track_uri, snap.state.track_uri) == 0));
		const char *lyrics_message = !snap.have_state
		                                  ? "Nothing playing"
		                              : g_lyrics_layout_error
		                                  ? "Not enough memory for lyrics layout"
		                              : g_lyrics_status.message[0]
		                                  ? g_lyrics_status.message
		                                  : NULL;
		const screen_lyrics_args lyrics_args = {
			.buf = textbuf,
			.tb = &g_tb,
			.doc = lyrics_document_ready ? &g_lyrics_payload.doc : NULL,
			.layout = lyrics_document_ready ? &g_lyrics_layout : NULL,
			.art = &g_art,
			.track = snap.have_state && snap.state.track[0]
			             ? snap.state.track
			             : "Lyrics",
			.elapsed_ms = progress,
			.duration_ms = duration,
			.highlight = lyrics_highlight,
			.loading = lyrics_loading,
			.error = lyrics_error,
			.status = lyrics_message,
			.loading_received = g_lyrics_status.bytes_received,
			.loading_total = g_lyrics_status.bytes_total,
			.loading_total_known = g_lyrics_status.bytes_total_known,
			.loading_animation_ms = (unsigned)osGetTime(),
			.scroll = g_lyrics_scroll,
			.follow = g_lyrics_follow,
			.pressed_id = touch.down ? touch.press_id : -1,
		};

		/* --- top screen ------------------------------------------------ */
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

		C2D_TargetClear(top, C2D_Color32(0, 0, 0, 0xFF));
		C2D_SceneBegin(top);

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
			.detail     = snap.fatal ? snap.status_detail : NULL,
		};
		screen_top_draw(&ta);

		/* --- bottom screen --------------------------------------------- */
		C2D_TargetClear(bottom, CLR_BOT_BG);
		C2D_SceneBegin(bottom);

		if (g_view == VIEW_LYRICS) {
			screen_lyrics_bottom_draw(&lyrics_args);
		} else if (g_view == VIEW_LIST) {
			recent_list *rl;
			playlist_list *pl;
			album_list *al;
			library_get_lists(&rl, &pl, &al);

			const screen_list_args la = {
				.buf        = textbuf,
				.tb         = &g_tb,
				.recents    = rl,
				.playlists  = pl,
				.albums     = al,
				.current_context_uri = snap.have_state
				                           ? current_collection_uri(&snap.state)
				                           : "",
				.search_query = g_list_search,
				.search_matches = pl->count + al->count,
				.playing     = playing,
				.animation_ms = (unsigned)osGetTime(),
				.elapsed_ms = progress,
				.duration_ms = duration,
				.scroll     = g_list_scroll,
				.pressed_id = touch.down ? touch.press_id : -1,
				.armed_id   = g_list_armed,
			};
			screen_list_draw(&la);
		} else if (g_view == VIEW_TRACKS) {
			worker_get_tracks(&g_tracks_buf);
			const bool search_done =
			    g_track_search_mode &&
			    g_track_search_status.state == TRACK_SEARCH_READY &&
			    g_track_search_applied_generation != 0;
			/* Matches are drawn the moment the first snapshot lands; the scan
			 * keeps running behind them. */
			const bool search_ready =
			    search_done || (g_track_search_mode &&
			                    g_track_search_applied_generation != 0 &&
			                    g_track_search_page.count > 0);
			const char *track_error = g_track_search_mode
			                              ? g_track_search_status.error
			                              : g_tracks_buf.error;
			const bool no_matches =
			    search_done && g_track_search_page.count == 0;
			const screen_tracks_args ta = {
				.buf = textbuf,
				.tb = &g_tb,
				.page = g_track_search_mode ? &g_track_search_page
				                            : &g_tracks_buf.page,
				.collection_name = g_tracks_collection.name,
				.back_label = g_tracks_return_view == VIEW_PLAYER ? "Player"
				                                                  : "Library",
				.current_track_uri =
				    snap.have_state ? snap.state.track_uri : "",
				.error = track_error,
				.playing = playing,
				.animation_ms = (unsigned)osGetTime(),
				.elapsed_ms = progress,
				.duration_ms = duration,
				.loading = g_track_search_mode
				               ? g_track_search_status.state == TRACK_SEARCH_LOADING
				               : g_tracks_buf.state == TRACKS_LOADING,
				.ready = g_track_search_mode ? search_ready
				                            : g_tracks_buf.state == TRACKS_READY,
				.search_mode = g_track_search_mode,
				.search_query = g_track_search_query,
				.search_scanned = g_track_search_status.scanned,
				.search_source_total = g_track_search_status.source_total,
				.search_matched_total = g_track_search_status.matched_total,
				.search_truncated = g_track_search_status.truncated,
				.search_animation_ms = (unsigned)osGetTime(),
				.no_matches = no_matches,
				.scroll = g_tracks_scroll,
				.pressed_id = touch.down ? touch.press_id : -1,
				.armed_id = g_tracks_armed,
			};
			screen_tracks_draw(&ta);
		} else {
				screen_player_args pa = {
				.buf         = textbuf,
				.tb          = &g_tb,
				.art         = &g_art,
				.track       = snap.have_state ? snap.state.track : NULL,
				.playing     = playing,
				.shuffle     = shuffled,
				.repeat      = effective_repeat(&snap),
				.progress_ms = progress,
				.duration_ms = duration,
				.pressed_id  = touch.down ? touch.press_id : -1,
				.scrubbing   = g_scrub == SCRUB_DRAGGING,
				.animation_ms = (unsigned)osGetTime(),
			};

			/* Asking every frame is the intended use: a hit is a short scan and
			 * a miss queues the fetch once. */
			recent_list *const rl = &g_recents_buf;
			const int          rn = worker_get_recents(rl);
			pa.shelf_count = rn <= 0 ? 0
			                 : rn < SHELF_TILES ? rn
			                                     : SHELF_TILES;
			for (int i = 0; i < pa.shelf_count; i++) {
				pa.shelf[i] = thumbs_get(rl->items[i].art_url);
				pa.shelf_current[i] =
				    snap.have_state &&
				    strcmp(rl->items[i].context_uri,
				           current_collection_uri(&snap.state)) == 0;
			}

			screen_player_draw(&pa);
		}

		const u64 volume_now = osGetTime();
		if (volume_now < g_volume_overlay_until) {
			const u64 remaining = g_volume_overlay_until - volume_now;
			const u8 alpha = remaining >= 350
			                     ? 255
			                     : (u8)(remaining * 255 / 350);
			const bool volume_supported =
			    snap.have_state && snap.state.supports_volume &&
			    snap.state.volume_known && snap.state.device_id[0];
			const volume_overlay_args va = {
				.buf = textbuf,
				.supported = volume_supported,
				.volume_percent = volume_supported ? effective_volume(&snap) : 0,
				.device_name = snap.have_state ? snap.state.device_name : NULL,
				.alpha = alpha,
			};
			volume_overlay_draw(&va);
		}
		if (g_smoketest && frames >= 300 && frames < 360) {
			const volume_overlay_args va = {
				.buf = textbuf,
				.supported = true,
				.volume_percent = 62,
				.device_name = "Test device",
				.alpha = 255,
			};
			volume_overlay_draw(&va);
			volume_overlay_supported_drawn = true;
		} else if (g_smoketest && frames >= 360 && frames < 420) {
			const volume_overlay_args va = {
				.buf = textbuf,
				.supported = true,
				.volume_percent = 0,
				.device_name = "Test device",
				.alpha = 255,
			};
			volume_overlay_draw(&va);
			volume_overlay_zero_drawn = true;
		} else if (g_smoketest && frames >= 420 && frames < 480) {
			const volume_overlay_args va = {
				.buf = textbuf,
				.supported = false,
				.volume_percent = 0,
				.device_name = "iPhone",
				.alpha = 255,
			};
			volume_overlay_draw(&va);
			volume_overlay_unsupported_drawn = true;
		}

		const size_t text_glyphs = C2D_TextBufGetNumGlyphs(textbuf);
		if (text_glyphs > max_text_glyphs)
			max_text_glyphs = text_glyphs;
		C3D_FrameEnd(0);

		/* The headless harness needs the app to exit on its own; a real console
		 * must not. So auto-exit is opt-in, enabled only by the presence of
		 * sdmc:/spotify/.smoketest (which dev.sh creates). */
		frames++;
		if (g_smoketest &&
		    ((frames >= 900 && tracks_probe_stage == 99 && !repeat_probe_at) ||
		     frames == 1800)) {
			if (frames == 1800 && tracks_probe_stage != 99)
				tl_step("tracks_timeout", 0, "stage=%d", tracks_probe_stage);
			tl_step("volume_overlay",
			        volume_overlay_supported_drawn &&
			            volume_overlay_zero_drawn &&
			            volume_overlay_unsupported_drawn,
			        "supported=%d zero=%d unsupported=%d",
			        (int)volume_overlay_supported_drawn,
			        (int)volume_overlay_zero_drawn,
			        (int)volume_overlay_unsupported_drawn);
			tl_step("text_buffer", max_text_glyphs < TEXTBUF_GLYPHS,
			        "peak=%u/%d glyphs", (unsigned)max_text_glyphs,
			        TEXTBUF_GLYPHS);
			tl_step("ui_loop", 1, "%d frames, art=%d", frames,
			        (int)g_art.valid);
			tl_done();
			break;
		}
	}

	worker_stop();
	lyrics_drop_local();
	worker_track_search_payload_free(&g_track_search_payload);
	art_free(&g_art);
	thumbs_free_all();
	net_exit();
	C2D_TextBufDelete(textbuf);
	ui_exit();
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return 0;
}
