#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>

#include "net/net.h"
#include "net/tls.h"
#include "testlog.h"

#define PHASE 2

/* Headless runs must not wait for input. Hold SELECT at boot to keep the
 * window open for screenshots instead. */
#define AUTO_EXIT_FRAMES 150

#define CLR_BG_TOP    C2D_Color32(0x18, 0x18, 0x18, 0xFF)
#define CLR_BG_BOTTOM C2D_Color32(0x10, 0x10, 0x10, 0xFF)
#define CLR_ACCENT    C2D_Color32(0x1D, 0xB9, 0x54, 0xFF)
#define CLR_TEXT      C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)

/* Drain the response until the peer closes. Enough for a spike; Phase 3
 * parses Content-Length / chunked encoding properly. */
static int tls_read_all(tls_conn *c, char *buf, int cap)
{
	int total = 0;
	while (total < cap - 1) {
		int n = tls_read(c, buf + total, cap - 1 - total);
		if (n <= 0)
			break;
		total += n;
	}
	buf[total] = '\0';
	return total;
}

/* Copy the HTTP status line ("HTTP/1.1 401 Unauthorized") out of a response. */
static void status_line(const char *resp, char *out, int outlen)
{
	const char *nl  = strchr(resp, '\r');
	int         len = nl ? (int)(nl - resp) : (int)strlen(resp);
	if (len > outlen - 1)
		len = outlen - 1;
	memcpy(out, resp, len);
	out[len] = '\0';
}

/* Handshake, issue one GET, and report the status line.
 * step_tls / step_http name the two verdicts this emits. */
static void probe(const char *host, const char *path, const char *step_tls,
                  const char *step_http, int expect_status)
{
	char      err[256];
	tls_conn *c = tls_connect(host, 443, err, sizeof err);
	if (!c) {
		tl_step(step_tls, 0, "%s: %s", host, err);
		return;
	}

	tl_step(step_tls, 1, "%s %s %s", host, tls_version(c), tls_ciphersuite(c));

	char req[512];
	snprintf(req, sizeof req,
	         "GET %s HTTP/1.1\r\n"
	         "Host: %s\r\n"
	         "Connection: close\r\n"
	         "User-Agent: spotify3ds/0.1\r\n"
	         "\r\n",
	         path, host);

	if (!tls_write(c, req, strlen(req))) {
		tl_step(step_http, 0, "write failed");
		tls_close(c);
		return;
	}

	static char resp[2048];
	int         n = tls_read_all(c, resp, sizeof resp);
	tls_close(c);

	if (n <= 0) {
		tl_step(step_http, 0, "no response");
		return;
	}

	char status[96];
	status_line(resp, status, sizeof status);

	/* An authenticated-endpoint 401 proves the full request/response path
	 * works end to end; we have no token yet. */
	char expect[8];
	snprintf(expect, sizeof expect, " %d ", expect_status);
	const bool ok = strstr(status, expect) != NULL;

	tl_step(step_http, ok, "bytes=%d status=%s", n, status);
}

static void run_spike(void)
{
	char err[128];

	if (!net_init(err, sizeof err)) {
		tl_step("net_init", 0, "%s", err);
		return;
	}
	tl_step("net_init", 1, "soc buffer 1MiB");

	/* RSA chain (DigiCert Global Root G2). Unauthenticated -> expect 401. */
	probe("api.spotify.com", "/v1/me", "tls_api", "http_api", 401);

	/* ECC chain (DigiCert Global Root G3) - an entirely separate code path
	 * in mbedTLS, so it must be proven independently. */
	probe("i.scdn.co", "/image/ab67616d0000b273ff9ca10b55ce82ae553c8228",
	      "tls_scdn", "http_scdn", 200);
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

	C2D_TextBuf textbuf = C2D_TextBufNew(256);
	C2D_Text    title, subtitle;
	C2D_TextParse(&title, textbuf, "Spotify Controller");
	C2D_TextParse(&subtitle, textbuf, "Phase 2 - TLS 1.2");
	C2D_TextOptimize(&title);
	C2D_TextOptimize(&subtitle);

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
		C2D_DrawText(&title, C2D_WithColor, 232.0f, 40.0f, 0.0f, 0.55f, 0.55f,
		             CLR_TEXT);
		C2D_DrawText(&subtitle, C2D_WithColor, 232.0f, 70.0f, 0.0f, 0.40f,
		             0.40f, CLR_TEXT);

		C2D_TargetClear(bottom, CLR_BG_BOTTOM);
		C2D_SceneBegin(bottom);
		C2D_DrawRectSolid(64.0f, 90.0f, 0.0f, 64.0f, 64.0f, CLR_ACCENT);
		C2D_DrawRectSolid(136.0f, 90.0f, 0.0f, 64.0f, 64.0f, CLR_ACCENT);
		C2D_DrawRectSolid(208.0f, 90.0f, 0.0f, 64.0f, 64.0f, CLR_ACCENT);
		C2D_DrawRectSolid(20.0f, 180.0f, 0.0f, 280.0f, 6.0f,
		                  C2D_Color32(0x40, 0x40, 0x40, 0xFF));

		C3D_FrameEnd(0);
	}

	net_exit();
	C2D_TextBufDelete(textbuf);
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return 0;
}
