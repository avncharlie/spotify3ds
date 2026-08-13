#pragma once

#include <citro2d.h>
#include <stdbool.h>

/* Shared drawing helpers and the type scale.
 *
 * The mockup specifies type in CSS pixels. citro2d's text scale is a float
 * multiplier on the system font, so the mapping has to be derived from the
 * font's own metrics rather than guessed - and the system font differs by
 * region and firmware, so it cannot be hardcoded either. ui_init() reads
 * fontGetInfo() once and fills the table; ui_font_probe() reports the numbers
 * so the arithmetic is checkable in a transcript.
 */

/* Type roles, named for what the mockup calls them rather than for a size. */
typedef enum {
	TY_TITLE_L = 0, /* 34px - art-hidden title */
	TY_TITLE,       /* 21px - title beside cover */
	TY_ARTIST_L,    /* 17px - art-hidden artist */
	TY_ALBUM_L,     /* 13px - art-hidden album */
	TY_ARTIST,      /* 12px */
	TY_ROW_NAME,    /* 11px - list row name, header */
	TY_ALBUM,       /* 10px */
	TY_ROW_SUB,     /* 8px  - list row subtitle, scrubber times */
	TY_MICRO,       /* 7px  - tracked mono-ish labels; see ui_micro_px() */
	TY_COUNT
} type_role;

/* Call once after C2D_Init. */
void ui_init(void);
void ui_exit(void);

/* Scale multiplier for a role, and the CSS px it was derived from. */
float ui_scale(type_role r);
float ui_px(type_role r);

/* The mockup asks for 7px micro-labels, which sit below the legibility floor:
 * the glyph atlas is minified ~4x and the filter turns it to mush. This
 * returns the size actually used so the choice lives in one place while it is
 * being evaluated. */
float ui_micro_px(void);
void  ui_set_micro_px(float px);

/* Baseline of the first line if its box starts at `top`. The mockup's gaps
 * read as visual gaps, so stack by baseline rather than by line box (which is
 * taller than the em and would space everything too loosely). */
float ui_baseline(float top, type_role r);

/* Draw text, truncating with an ellipsis if it exceeds maxw. */
void ui_text(C2D_TextBuf buf, const char *s, float x, float y, type_role r,
             float maxw, u32 clr);

/* Draw text broken across up to maxlines lines at word boundaries, and return
 * the baseline of the last line drawn. `leading` multiplies ui_px(r) to get the
 * baseline step. Unlike ui_text this is for prose that must be read in full -
 * error messages - rather than for a name that may be ellipsised. Text still
 * remaining on the last permitted line is ellipsised as usual. */
float ui_text_wrapped(C2D_TextBuf buf, const char *s, float x, float y,
                      type_role r, float maxw, u32 clr, int maxlines,
                      float leading);

/* Draw one case-insensitive matched substring in a second colour. Falls back
 * to ui_text when needle is empty or absent. */
void ui_text_highlight(C2D_TextBuf buf, const char *s, const char *needle,
                       float x, float y, type_role r, float maxw, u32 clr,
                       u32 highlight_clr);

/* Draw text with per-glyph letter-spacing, for the mockup's tracked micro
 * labels ("RECENTLY PLAYED", "IPHONE"). citro2d has no tracking parameter, so
 * this advances glyph by glyph. Only worth it for short labels. */
void ui_text_tracked(C2D_TextBuf buf, const char *s, float x, float y,
                     type_role r, float tracking_px, u32 clr);

/* Width the string would occupy, for right-aligning and centring. */
float ui_text_width(C2D_TextBuf buf, const char *s, type_role r);

/* Filled circle. citro2d has no circle primitive; this is a triangle fan, and
 * 16 segments at r=20 keeps the chord error under half a pixel. */
void ui_disc(float cx, float cy, float r, u32 clr);

/* Darken cover art and draw the four-bar current-playback indicator. Active
 * playback animates slowly; paused playback remains at a fixed silhouette. */
void ui_now_playing_badge(float x, float y, float size, bool playing,
                          unsigned animation_ms);

/* Emit font metrics and the derived scales. Smoketest only; no-op unless
 * timing output is enabled. */
void ui_font_probe(void);
