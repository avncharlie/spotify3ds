#pragma once

#include <citro2d.h>
#include <stdbool.h>

typedef struct {
	C2D_TextBuf buf;
	bool        supported;
	int         volume_percent;
	const char *device_name;
	u8          alpha;
} volume_overlay_args;

/* Draw above the completed bottom-screen view. The overlay has no touch area. */
void volume_overlay_draw(const volume_overlay_args *a);
