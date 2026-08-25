#pragma once

#include <citro2d.h>

#include "touch.h"

enum {
	SETUP_BTN_SCAN = 1900,
	SETUP_BTN_COMPLETE,
};

typedef struct {
	C2D_TextBuf   buf;
	touch_builder *tb;
	const char    *message;
	int            pressed_id;
} screen_setup_args;

void screen_setup_draw(const screen_setup_args *args);
void screen_setup_complete_draw(C2D_TextBuf buf, touch_builder *tb,
                                int pressed_id);
