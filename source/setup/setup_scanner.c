#include "setup_scanner.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quirc.h"

#define CAMERA_WIDTH  400
#define CAMERA_HEIGHT 240
#define CAMERA_BYTES  (CAMERA_WIDTH * CAMERA_HEIGHT * 2)
#define CAMERA_TIMEOUT_NS (2ULL * 1000 * 1000 * 1000)

static void preview_rgb565(const u16 *image)
{
	u8 *framebuffer = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
	for (int y = 0; y < CAMERA_HEIGHT; y++) {
		for (int x = 0; x < CAMERA_WIDTH; x++) {
			const u16 pixel = image[y * CAMERA_WIDTH + x];
			const int draw_y = CAMERA_HEIGHT - 1 - y;
			const size_t at = (size_t)(draw_y + x * CAMERA_HEIGHT) * 3;
			framebuffer[at] = (u8)((pixel & 0x1F) << 3);
			framebuffer[at + 1] = (u8)(((pixel >> 5) & 0x3F) << 2);
			framebuffer[at + 2] = (u8)(((pixel >> 11) & 0x1F) << 3);
		}
	}
	gfxFlushBuffers();
	gspWaitForVBlank();
	gfxSwapBuffersGpu();
}

static Result camera_start(u32 *transfer_bytes)
{
	Result result = camInit();
	if (R_FAILED(result))
		return result;
	if (R_FAILED(result = CAMU_GetMaxBytes(transfer_bytes, CAMERA_WIDTH,
	                                      CAMERA_HEIGHT)) ||
	    R_FAILED(result = CAMU_SetSize(SELECT_OUT1, SIZE_CTR_TOP_LCD, CONTEXT_A)) ||
	    R_FAILED(result = CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565,
	                                          CONTEXT_A)) ||
	    R_FAILED(result = CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_15)) ||
	    R_FAILED(result = CAMU_SetNoiseFilter(SELECT_OUT1, true)) ||
	    R_FAILED(result = CAMU_SetAutoExposure(SELECT_OUT1, true)) ||
	    R_FAILED(result = CAMU_SetAutoWhiteBalance(SELECT_OUT1, true)) ||
	    R_FAILED(result = CAMU_SetTransferBytes(PORT_CAM1, *transfer_bytes,
	                                            CAMERA_WIDTH, CAMERA_HEIGHT)) ||
	    R_FAILED(result = CAMU_Activate(SELECT_OUT1)) ||
	    R_FAILED(result = CAMU_ClearBuffer(PORT_CAM1)) ||
	    R_FAILED(result = CAMU_StartCapture(PORT_CAM1))) {
		CAMU_Activate(SELECT_NONE);
		camExit();
		return result;
	}
	return 0;
}

static void camera_stop(void)
{
	CAMU_StopCapture(PORT_CAM1);
	CAMU_Activate(SELECT_NONE);
	camExit();
}

bool setup_scanner_run(setup_credentials *out, char *err, int errlen)
{
	bool success = false;
	bool invalid = false;
	u16 *camera = calloc(CAMERA_WIDTH * CAMERA_HEIGHT, sizeof *camera);
	struct quirc *decoder = quirc_new();
	struct quirc_code *code = malloc(sizeof *code);
	struct quirc_data *data = malloc(sizeof *data);
	if (!camera || !decoder || !code || !data ||
	    quirc_resize(decoder, CAMERA_WIDTH, CAMERA_HEIGHT) != 0) {
		snprintf(err, errlen, "Not enough memory for QR scanner");
		goto cleanup;
	}
	u32 transfer_bytes = 0;
	Result result = camera_start(&transfer_bytes);
	if (R_FAILED(result)) {
		snprintf(err, errlen, "Camera start failed: 0x%08lX",
		         (unsigned long)result);
		goto cleanup;
	}

	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_B) {
			snprintf(err, errlen, "Setup scan cancelled");
			break;
		}
		Handle event = 0;
		result = CAMU_SetReceiving(&event, camera, PORT_CAM1, CAMERA_BYTES,
		                           (s16)transfer_bytes);
		if (R_FAILED(result) ||
		    R_FAILED(result = svcWaitSynchronization(event, CAMERA_TIMEOUT_NS))) {
			if (event)
				svcCloseHandle(event);
			snprintf(err, errlen, "Camera capture failed: 0x%08lX",
			         (unsigned long)result);
			break;
		}
		svcCloseHandle(event);
		result = CAMU_StopCapture(PORT_CAM1);
		if (R_FAILED(result)) {
			snprintf(err, errlen, "Camera pause failed: 0x%08lX",
			         (unsigned long)result);
			break;
		}
		preview_rgb565(camera);

		int width, height;
		u8 *gray = quirc_begin(decoder, &width, &height);
		for (int i = 0; i < width * height; i++) {
			const u16 pixel = camera[i];
			gray[i] = (u8)(((((pixel >> 11) & 0x1F) << 3) +
			                (((pixel >> 5) & 0x3F) << 2) +
			                ((pixel & 0x1F) << 3)) / 3);
		}
		quirc_end(decoder);
		const int count = quirc_count(decoder);
		for (int i = 0; i < count; i++) {
			quirc_extract(decoder, i, code);
			quirc_decode_error_t decode = quirc_decode(code, data);
			if (decode == QUIRC_ERROR_DATA_ECC) {
				quirc_flip(code);
				decode = quirc_decode(code, data);
			}
			if (decode != QUIRC_SUCCESS)
				continue;
			if (setup_qr_parse(data->payload, (size_t)data->payload_len, out, err,
			                   errlen))
				success = true;
			else {
				snprintf(err, errlen, "Couldn't recognize QR code.");
				invalid = true;
			}
			break;
		}
		if (success || invalid)
			break;
		if (R_FAILED(result = CAMU_ClearBuffer(PORT_CAM1)) ||
		    R_FAILED(result = CAMU_StartCapture(PORT_CAM1))) {
			snprintf(err, errlen, "Camera restart failed: 0x%08lX",
			         (unsigned long)result);
			break;
		}
	}
	camera_stop();

cleanup:
	if (camera)
		memset(camera, 0, CAMERA_BYTES);
	if (data)
		memset(data, 0, sizeof *data);
	free(camera);
	free(code);
	free(data);
	quirc_destroy(decoder);
	return success;
}
