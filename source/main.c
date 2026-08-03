#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>

#include "net/net.h"
#include "spotify/auth.h"
#include "spotify/player.h"
#include "testlog.h"

#define PHASE 4

/* Headless runs must not wait for input. Hold SELECT at boot to keep the
 * window open for screenshots instead. */
#define AUTO_EXIT_FRAMES 150

#define CLR_BG_TOP    C2D_Color32(0x18, 0x18, 0x18, 0xFF)
#define CLR_BG_BOTTOM C2D_Color32(0x10, 0x10, 0x10, 0xFF)
#define CLR_ACCENT    C2D_Color32(0x1D, 0xB9, 0x54, 0xFF)
#define CLR_TEXT      C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)

static player_state g_state;
static bool         g_have_state;

/* Spotify acknowledges a command before the device reflects it, so a poll
 * issued immediately after still reports the old state. Give it a moment. */
static void settle(void)
{
	svcSleepThread(1500ull * 1000 * 1000); /* 1.5s */
}

/* Poll and report is_playing, or -1 when nothing is playing. */
static int observe(const char *label, player_state *out)
{
	char          err[256];
	player_state  st;
	player_result pr = player_poll(&st, err, sizeof err);

	if (pr == PLAYER_OK) {
		if (out)
			*out = st;
		tl_log("%s: playing=%d pos=%ldms track=%s", label, (int)st.is_playing,
		       st.progress_ms, st.track);
		return st.is_playing ? 1 : 0;
	}

	tl_log("%s: %s", label, player_result_str(pr));
	return -1;
}

static void run_spike(void)
{
	char err[256];

	if (!net_init(err, sizeof err)) {
		tl_step("net_init", 0, "%s", err);
		return;
	}
	if (!auth_load(err, sizeof err)) {
		tl_step("creds_load", 0, "%s", err);
		return;
	}
	if (!auth_token(err, sizeof err)) {
		tl_step("token", 0, "%s", err);
		return;
	}
	tl_step("auth", 1, "token acquired");

	/* --- baseline ---------------------------------------------------- */
	player_state before;
	int          was_playing = observe("baseline", &before);
	if (was_playing < 0) {
		tl_step("baseline", 0,
		        "nothing playing - start Spotify on a device first");
		return;
	}
	g_state      = before;
	g_have_state = true;
	tl_step("baseline", 1, "playing=%d track=%s", was_playing, before.track);

	/* --- pause -------------------------------------------------------- */
	player_result pr = player_pause(err, sizeof err);
	if (pr != PLAYER_OK) {
		tl_step("pause", 0, "%s: %s", player_result_str(pr), err);
		return;
	}
	settle();
	int after_pause = observe("after_pause", NULL);
	tl_step("pause", after_pause == 0, "204 accepted, observed playing=%d",
	        after_pause);

	/* --- play --------------------------------------------------------- */
	pr = player_play(err, sizeof err);
	if (pr != PLAYER_OK) {
		tl_step("play", 0, "%s: %s", player_result_str(pr), err);
		return;
	}
	settle();
	int after_play = observe("after_play", NULL);
	tl_step("play", after_play == 1, "204 accepted, observed playing=%d",
	        after_play);

	/* --- next --------------------------------------------------------- */
	pr = player_next(err, sizeof err);
	if (pr != PLAYER_OK) {
		tl_step("next", 0, "%s: %s", player_result_str(pr), err);
		return;
	}
	settle();
	player_state after_next;
	memset(&after_next, 0, sizeof after_next);
	observe("after_next", &after_next);
	/* Track name changing is the real proof the skip took effect. */
	tl_step("next", strcmp(after_next.track, before.track) != 0,
	        "\"%s\" -> \"%s\"", before.track, after_next.track);

	/* --- previous: return to where we started ------------------------- */
	pr = player_prev(err, sizeof err);
	if (pr != PLAYER_OK) {
		tl_step("prev", 0, "%s: %s", player_result_str(pr), err);
		return;
	}
	settle();
	player_state after_prev;
	memset(&after_prev, 0, sizeof after_prev);
	observe("after_prev", &after_prev);
	tl_step("prev", 1, "now \"%s\"", after_prev.track);

	/* --- seek ---------------------------------------------------------- */
	const long target = 30000; /* 30s in */
	pr = player_seek(target, err, sizeof err);
	if (pr != PLAYER_OK) {
		tl_step("seek", 0, "%s: %s", player_result_str(pr), err);
		return;
	}
	settle();
	player_state after_seek;
	memset(&after_seek, 0, sizeof after_seek);
	observe("after_seek", &after_seek);
	/* Playback keeps advancing, so accept a window rather than an exact ms. */
	const long delta = after_seek.progress_ms - target;
	tl_step("seek", delta >= -2000 && delta <= 8000,
	        "requested %ldms, observed %ldms (delta %ldms)", target,
	        after_seek.progress_ms, delta);

	/* Leave playback as we found it. */
	if (was_playing == 0)
		player_pause(err, sizeof err);

	g_state      = after_seek;
	g_have_state = true;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();

	tl_init(PHASE);

	C3D_RenderTarget *top    = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

	run_spike();
	tl_done();

	C2D_TextBuf textbuf = C2D_TextBufNew(1024);
	C2D_Text    t_track, t_artist, t_album;
	C2D_TextParse(&t_track, textbuf,
	              g_have_state ? g_state.track : "Nothing playing");
	C2D_TextParse(&t_artist, textbuf, g_have_state ? g_state.artist : "");
	C2D_TextParse(&t_album, textbuf, g_have_state ? g_state.album : "");
	C2D_TextOptimize(&t_track);
	C2D_TextOptimize(&t_artist);
	C2D_TextOptimize(&t_album);

	hidScanInput();
	const bool hold_open = (hidKeysHeld() & KEY_SELECT) != 0;

	int frames = 0;
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		if (!hold_open && ++frames > AUTO_EXIT_FRAMES)
			break;

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

		C2D_TargetClear(top, CLR_BG_TOP);
		C2D_SceneBegin(top);
		C2D_DrawRectSolid(16.0f, 20.0f, 0.0f, 200.0f, 200.0f, CLR_ACCENT);
		C2D_DrawText(&t_track, C2D_WithColor, 232.0f, 40.0f, 0.0f, 0.50f, 0.50f,
		             CLR_TEXT);
		C2D_DrawText(&t_artist, C2D_WithColor, 232.0f, 70.0f, 0.0f, 0.40f,
		             0.40f, CLR_TEXT);
		C2D_DrawText(&t_album, C2D_WithColor, 232.0f, 95.0f, 0.0f, 0.35f, 0.35f,
		             C2D_Color32(0xA0, 0xA0, 0xA0, 0xFF));

		C2D_TargetClear(bottom, CLR_BG_BOTTOM);
		C2D_SceneBegin(bottom);
		C2D_DrawRectSolid(64.0f, 90.0f, 0.0f, 64.0f, 64.0f, CLR_ACCENT);
		C2D_DrawRectSolid(136.0f, 90.0f, 0.0f, 64.0f, 64.0f, CLR_ACCENT);
		C2D_DrawRectSolid(208.0f, 90.0f, 0.0f, 64.0f, 64.0f, CLR_ACCENT);
		C2D_DrawRectSolid(20.0f, 180.0f, 0.0f, 280.0f, 6.0f,
		                  C2D_Color32(0x40, 0x40, 0x40, 0xFF));
		if (g_have_state && g_state.duration_ms > 0) {
			float frac = (float)g_state.progress_ms / (float)g_state.duration_ms;
			if (frac > 1.0f)
				frac = 1.0f;
			C2D_DrawRectSolid(20.0f, 180.0f, 0.0f, 280.0f * frac, 6.0f,
			                  CLR_ACCENT);
		}

		C3D_FrameEnd(0);
	}

	net_exit();
	C2D_TextBufDelete(textbuf);
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return 0;
}
