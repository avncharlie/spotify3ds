#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>

#include "net/net.h"
#include "spotify/auth.h"
#include "spotify/player.h"
#include "testlog.h"

#define PHASE 3

/* Headless runs must not wait for input. Hold SELECT at boot to keep the
 * window open for screenshots instead. */
#define AUTO_EXIT_FRAMES 150

#define CLR_BG_TOP    C2D_Color32(0x18, 0x18, 0x18, 0xFF)
#define CLR_BG_BOTTOM C2D_Color32(0x10, 0x10, 0x10, 0xFF)
#define CLR_ACCENT    C2D_Color32(0x1D, 0xB9, 0x54, 0xFF)
#define CLR_TEXT      C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)

static player_state g_state;
static bool         g_have_state;

static void run_spike(void)
{
	char err[256];

	if (!net_init(err, sizeof err)) {
		tl_step("net_init", 0, "%s", err);
		return;
	}
	tl_step("net_init", 1, "soc buffer 1MiB");

	/* --- credentials ------------------------------------------------- */
	if (!auth_load(err, sizeof err)) {
		tl_step("creds_load", 0, "%s", err);
		return;
	}
	tl_step("creds_load", 1, "sdmc:/spotify/creds.cfg");

	/* --- refresh grant ----------------------------------------------- */
	const char *token = auth_token(err, sizeof err);
	if (!token) {
		tl_step("token_refresh", 0, "%s", err);
		return;
	}
	/* Log only a prefix: the full token is a bearer credential. */
	tl_step("token_refresh", 1, "len=%u prefix=%.8s", (unsigned)strlen(token),
	        token);

	/* --- poll --------------------------------------------------------- */
	player_state  st;
	player_result pr = player_poll(&st, err, sizeof err);

	if (pr == PLAYER_OK) {
		g_state      = st;
		g_have_state = true;
		tl_step("poll", 1, "track=%s | artist=%s | album=%s", st.track,
		        st.artist, st.album);
		tl_step("poll_fields", st.duration_ms > 0,
		        "progress=%ldms duration=%ldms playing=%d art=%.48s",
		        st.progress_ms, st.duration_ms, (int)st.is_playing,
		        st.art_url[0] ? st.art_url : "(none)");
	} else if (pr == PLAYER_NOTHING_PLAYING) {
		/* A normal state, not a failure: nothing is playing right now. The
		 * auth chain above is what this phase set out to prove. */
		tl_step("poll", 1, "204 nothing playing (start Spotify to see a track)");
	} else {
		tl_step("poll", 0, "%s: %s", player_result_str(pr), err);
	}
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

		/* Progress bar reflecting the real poll, when we have one. */
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
