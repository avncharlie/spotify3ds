#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>

#include "net/net.h"
#include "spotify/auth.h"
#include "spotify/art.h"
#include "spotify/player.h"
#include "testlog.h"

#define PHASE 5

/* Headless runs must not wait for input. Hold SELECT at boot to keep the
 * window open for screenshots instead. */
#define AUTO_EXIT_FRAMES 150

#define CLR_BG_TOP    C2D_Color32(0x18, 0x18, 0x18, 0xFF)
#define CLR_BG_BOTTOM C2D_Color32(0x10, 0x10, 0x10, 0xFF)
#define CLR_ACCENT    C2D_Color32(0x1D, 0xB9, 0x54, 0xFF)
#define CLR_TEXT      C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)

static player_state g_state;
static bool         g_have_state;

static album_art g_art;

static void run_spike(void)
{
	char err[256];

	if (!net_init(err, sizeof err)) {
		tl_step("net_init", 0, "%s", err);
		return;
	}
	if (!auth_load(err, sizeof err) || !auth_token(err, sizeof err)) {
		tl_step("auth", 0, "%s", err);
		return;
	}
	tl_step("auth", 1, "token acquired");

	player_state  st;
	player_result pr = player_poll(&st, err, sizeof err);
	if (pr != PLAYER_OK) {
		tl_step("poll", 0, "%s", player_result_str(pr));
		return;
	}
	g_state      = st;
	g_have_state = true;
	tl_step("poll", 1, "%s - %s", st.track, st.artist);

	if (!st.art_url[0]) {
		tl_step("art_url", 0, "no art url in response");
		return;
	}
	tl_step("art_url", 1, "%.64s", st.art_url);

	/* --- fetch + decode + upload -------------------------------------- */
	if (!art_load(&g_art, st.art_url, err, sizeof err)) {
		tl_step("art_load", 0, "%s", err);
		return;
	}
	tl_step("art_load", g_art.valid, "decoded %dx%d in %ums", g_art.src_w,
	        g_art.src_h, g_art.decode_ms);

	/* Expect 640x640 at 1/4 scale. */
	tl_step("art_size", g_art.src_w == 160 && g_art.src_h == 160,
	        "%dx%d (expected 160x160)", g_art.src_w, g_art.src_h);

	/* UVs must address exactly the populated corner of the 256x256 texture. */
	const float want = 160.0f / 256.0f;
	const bool  uv_ok = g_art.sub.right > want - 0.01f &&
	                   g_art.sub.right < want + 0.01f;
	tl_step("art_uv", uv_ok, "right=%.4f bottom=%.4f (expect %.4f / %.4f)",
	        g_art.sub.right, g_art.sub.bottom, want, 1.0f - want);

	/* Second call with the same URL must not refetch. */
	const unsigned before = g_art.decode_ms;
	art_load(&g_art, st.art_url, err, sizeof err);
	tl_step("art_cache", g_art.decode_ms == before,
	        "same url reused (decode_ms unchanged=%u)", g_art.decode_ms);
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
		if (g_art.valid)
			C2D_DrawImageAt(g_art.image, 16.0f, 20.0f, 0.0f, NULL, 1.25f,
			                1.25f);
		else
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
