#include <3ds.h>
#include <citro2d.h>
#include <mbedtls/version.h>
#include <stdio.h>

#include "testlog.h"

#define PHASE 0

/* Headless runs (the normal dev loop) must not wait for a button press.
 * Hold SELECT at boot to keep the window open for eyeballing instead. */
#define AUTO_EXIT_FRAMES 150

#define CLR_BG_TOP    C2D_Color32(0x18, 0x18, 0x18, 0xFF)
#define CLR_BG_BOTTOM C2D_Color32(0x10, 0x10, 0x10, 0xFF)
#define CLR_ACCENT    C2D_Color32(0x1D, 0xB9, 0x54, 0xFF) /* Spotify green */
#define CLR_TEXT      C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)

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

	tl_step("gfx_init", top && bottom, "top=%p bottom=%p", (void *)top,
	        (void *)bottom);

	/* Proves the mbedTLS headers/libs are actually linked in, not just that
	 * the build didn't break. Phase 2 does the real handshake. */
	char mbed_ver[32];
	mbedtls_version_get_string(mbed_ver);
	tl_step("mbedtls_link", 1, "version=%s", mbed_ver);

	/* Text rendering uses the system font (already resident in VRAM), so
	 * there is no font asset to load or fail on. */
	C2D_TextBuf textbuf = C2D_TextBufNew(256);
	C2D_Text    title, subtitle, hint;

	C2D_TextParse(&title, textbuf, "Spotify Controller");
	C2D_TextParse(&subtitle, textbuf, "Phase 0 - build harness OK");
	C2D_TextParse(&hint, textbuf, "START to exit");
	C2D_TextOptimize(&title);
	C2D_TextOptimize(&subtitle);
	C2D_TextOptimize(&hint);

	tl_step("text_parse", 1, "system font");

	hidScanInput();
	const bool hold_open = (hidKeysHeld() & KEY_SELECT) != 0;
	tl_log("hold_open=%d", (int)hold_open);

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
		/* Placeholder for the 200x200 album cover (Phase 5). */
		C2D_DrawRectSolid(16.0f, 20.0f, 0.0f, 200.0f, 200.0f, CLR_ACCENT);
		C2D_DrawText(&title, C2D_WithColor, 232.0f, 40.0f, 0.0f, 0.55f, 0.55f,
		             CLR_TEXT);
		C2D_DrawText(&subtitle, C2D_WithColor, 232.0f, 70.0f, 0.0f, 0.40f,
		             0.40f, CLR_TEXT);

		C2D_TargetClear(bottom, CLR_BG_BOTTOM);
		C2D_SceneBegin(bottom);
		/* Placeholder transport row (Phase 6 makes these real buttons). */
		C2D_DrawRectSolid(64.0f, 90.0f, 0.0f, 64.0f, 64.0f, CLR_ACCENT);
		C2D_DrawRectSolid(136.0f, 90.0f, 0.0f, 64.0f, 64.0f, CLR_ACCENT);
		C2D_DrawRectSolid(208.0f, 90.0f, 0.0f, 64.0f, 64.0f, CLR_ACCENT);
		/* Placeholder scrubber track. */
		C2D_DrawRectSolid(20.0f, 180.0f, 0.0f, 280.0f, 6.0f,
		                  C2D_Color32(0x40, 0x40, 0x40, 0xFF));
		C2D_DrawText(&hint, C2D_WithColor, 20.0f, 205.0f, 0.0f, 0.40f, 0.40f,
		             CLR_TEXT);

		C3D_FrameEnd(0);
	}

	tl_step("render_loop", 1, "frames=%d", frames);
	tl_done();

	C2D_TextBufDelete(textbuf);
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return 0;
}
