#include <3ds.h>
#include <citro2d.h>
#include <mbedtls/version.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net/net.h"
#include "testlog.h"

#define PHASE 1

/* Headless runs must not wait for input. Hold SELECT at boot to keep the
 * window open for screenshots instead. */
#define AUTO_EXIT_FRAMES 150

#define CLR_BG_TOP    C2D_Color32(0x18, 0x18, 0x18, 0xFF)
#define CLR_BG_BOTTOM C2D_Color32(0x10, 0x10, 0x10, 0xFF)
#define CLR_ACCENT    C2D_Color32(0x1D, 0xB9, 0x54, 0xFF)
#define CLR_TEXT      C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)

/* Read until the peer closes or the buffer fills. Fine for a spike; the real
 * client (Phase 2+) parses Content-Length / chunked encoding properly. */
static int read_all(int fd, char *buf, int cap)
{
	int total = 0;
	while (total < cap - 1) {
		int n = recv(fd, buf + total, cap - 1 - total, 0);
		if (n <= 0)
			break;
		total += n;
	}
	buf[total] = '\0';
	return total;
}

static void run_spike(void)
{
	char err[128];

	/* --- sockets up ------------------------------------------------- */
	if (!net_init(err, sizeof err)) {
		tl_step("net_init", 0, "%s", err);
		return;
	}
	tl_step("net_init", 1, "soc buffer 1MiB");

	/* --- DNS -------------------------------------------------------- */
	char ip[64];
	if (net_resolve("api.spotify.com", ip, sizeof ip))
		tl_step("dns", 1, "api.spotify.com=%s", ip);
	else
		tl_step("dns", 0, "gethostbyname failed");

	/* --- TCP to the real API port ----------------------------------- */
	int fd = net_tcp_connect("api.spotify.com", 443, err, sizeof err);
	if (fd >= 0) {
		tl_step("tcp_connect", 1, "api.spotify.com:443 fd=%d", fd);
		net_close(fd);
	} else {
		tl_step("tcp_connect", 0, "%s", err);
	}

	/* --- plain HTTP round trip, proving bidirectional data flow ------ */
	fd = net_tcp_connect("example.com", 80, err, sizeof err);
	if (fd < 0) {
		tl_step("http_get", 0, "%s", err);
		return;
	}

	static const char req[] = "GET / HTTP/1.1\r\n"
	                          "Host: example.com\r\n"
	                          "Connection: close\r\n"
	                          "User-Agent: spotify3ds/0.1\r\n"
	                          "\r\n";

	if (send(fd, req, strlen(req), 0) < 0) {
		tl_step("http_get", 0, "send failed");
		net_close(fd);
		return;
	}

	static char resp[4096];
	int         n = read_all(fd, resp, sizeof resp);
	net_close(fd);

	if (n <= 0) {
		tl_step("http_get", 0, "no response bytes");
		return;
	}

	/* First line looks like "HTTP/1.1 200 OK" */
	char        status[64] = {0};
	const char *nl         = strchr(resp, '\r');
	int         len        = nl ? (int)(nl - resp) : 0;
	if (len > (int)sizeof status - 1)
		len = sizeof status - 1;
	if (len > 0)
		memcpy(status, resp, len);

	tl_step("http_get", strncmp(resp, "HTTP/1.", 7) == 0, "bytes=%d status=%s",
	        n, status[0] ? status : "(none)");
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

	char mbed_ver[32];
	mbedtls_version_get_string(mbed_ver);
	tl_log("mbedtls=%s", mbed_ver);

	run_spike();
	tl_done();

	C2D_TextBuf textbuf = C2D_TextBufNew(256);
	C2D_Text    title, subtitle;
	C2D_TextParse(&title, textbuf, "Spotify Controller");
	C2D_TextParse(&subtitle, textbuf, "Phase 1 - socket spike");
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
