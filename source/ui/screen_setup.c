#include "screen_setup.h"

#include "ui.h"

#define CLR_BG C2D_Color32(0x08, 0x08, 0x08, 0xFF)
#define CLR_HEADER C2D_Color32(0x11, 0x11, 0x11, 0xFF)
#define CLR_TEXT C2D_Color32(0xE8, 0xE8, 0xE8, 0xFF)
#define CLR_SUB C2D_Color32(0xBC, 0xBC, 0xC0, 0xFF)
#define CLR_MUTED C2D_Color32(0x72, 0x72, 0x76, 0xFF)
#define CLR_DIM C2D_Color32(0x55, 0x55, 0x59, 0xFF)
#define CLR_GREEN C2D_Color32(0x1D, 0xB9, 0x54, 0xFF)
#define CLR_PRESS C2D_Color32(0x28, 0xD8, 0x68, 0xFF)

void screen_setup_draw(const screen_setup_args *args)
{
	C2D_DrawRectSolid(0, 0, 0, 320, 240, CLR_BG);
	C2D_DrawRectSolid(0, 0, 0, 320, 30, CLR_HEADER);
	ui_text(args->buf, "Set up Spotify3DS", 16,
	        ui_baseline((30 - ui_px(TY_ROW_NAME)) / 2, TY_ROW_NAME), TY_ROW_NAME,
	        288, CLR_TEXT);

	ui_text_tracked(args->buf, "ON YOUR COMPUTER, OPEN", 20,
	                ui_baseline(43, TY_MICRO), TY_MICRO, 1.0f, CLR_MUTED);
	ui_text(args->buf,
	        "github.com/avncharlie/spotify3ds/releases/latest", 20,
	        ui_baseline(67, TY_ROW_NAME), TY_ROW_NAME, 300, CLR_GREEN);

	ui_text(args->buf, "1", 20, ui_baseline(97, TY_ROW_SUB), TY_ROW_SUB, 10,
	        CLR_DIM);
	ui_text(args->buf, "Download and run the setup app for your system.", 33,
	        ui_baseline(97, TY_ROW_SUB), TY_ROW_SUB, 267, CLR_SUB);
	ui_text(args->buf, "2", 20, ui_baseline(120, TY_ROW_SUB), TY_ROW_SUB, 10,
	        CLR_DIM);
	ui_text(args->buf, "Authorize Spotify, then scan the code it shows.", 33,
	        ui_baseline(120, TY_ROW_SUB), TY_ROW_SUB, 267, CLR_SUB);
	ui_text(args->buf, "Prefer the terminal?", 20,
	        ui_baseline(146, TY_MICRO), TY_MICRO, 280, CLR_DIM);
	ui_text(args->buf, "Run bootstrap_auth.py instead - see README.", 20,
	        ui_baseline(161, TY_MICRO), TY_MICRO, 280, CLR_DIM);

	if (args->message && args->message[0])
		ui_text_wrapped(args->buf, args->message, 16,
		                ui_baseline(179, TY_MICRO), TY_MICRO, 288, CLR_SUB, 2,
		                1.15f);
	const u32 button = args->pressed_id == SETUP_BTN_SCAN ? CLR_PRESS : CLR_GREEN;
	C2D_DrawRectSolid(0, 208, 0, 320, 32, button);
	const char *action = "START  Scan setup code";
	const float width = ui_text_width(args->buf, action, TY_ROW_NAME);
	ui_text(args->buf, action, (320 - width) / 2,
	        ui_baseline(217, TY_ROW_NAME), TY_ROW_NAME, width, CLR_BG);
	tb_add(args->tb, 0, 208, 320, 32, SETUP_BTN_SCAN);
}

void screen_setup_complete_draw(C2D_TextBuf buf, touch_builder *tb,
                                int pressed_id)
{
	C2D_DrawRectSolid(0, 0, 0, 320, 240, CLR_BG);
	C2D_DrawRectSolid(0, 0, 0, 320, 30, CLR_HEADER);
	ui_text(buf, "Setup complete", 16,
	        ui_baseline((30 - ui_px(TY_ROW_NAME)) / 2, TY_ROW_NAME), TY_ROW_NAME,
	        288, CLR_TEXT);

	ui_text(buf, "You can close the setup app on your computer.", 16,
	        ui_baseline(57, TY_ROW_SUB), TY_ROW_SUB, 288, CLR_TEXT);
	ui_text(buf, "Spotify3DS will sign itself in from now on.", 16,
	        ui_baseline(78, TY_ROW_SUB), TY_ROW_SUB, 288, CLR_TEXT);
	ui_text(buf, "sdmc:/spotify/creds.cfg", 16,
	        ui_baseline(103, TY_MICRO), TY_MICRO, 288, CLR_DIM);

	const u32 button = pressed_id == SETUP_BTN_COMPLETE ? CLR_PRESS : CLR_GREEN;
	C2D_DrawRectSolid(0, 208, 0, 320, 32, button);
	const char *action = "A  Start listening";
	const float width = ui_text_width(buf, action, TY_ROW_NAME);
	ui_text(buf, action, (320 - width) / 2, ui_baseline(217, TY_ROW_NAME),
	        TY_ROW_NAME, width, CLR_BG);
	tb_add(tb, 0, 208, 320, 32, SETUP_BTN_COMPLETE);
}
