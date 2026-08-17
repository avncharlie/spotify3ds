#include "screen_lyrics.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

#define BOT_W 320.0f
#define BOT_H 240.0f

#define HEADER_H 30.0f
#define CONTENT_BOTTOM (BOT_H - UI_PROGRESS_BAR_H)

/* These values are deliberately kept together: they are adjusted against a
 * real bottom screen rather than derived from the rest of the application. */
#define FULL_X             4.0f
#define FULL_W             288.0f
#define FULL_TIME_W        30.0f
#define TIMESTAMP_TEXT_GAP 5.0f
#define FULL_TEXT_X        (FULL_X + FULL_TIME_W + TIMESTAMP_TEXT_GAP)
#define FULL_TEXT_W        (FULL_W - FULL_TIME_W - TIMESTAMP_TEXT_GAP)
#define FULL_LEADING       20.0f
#define FULL_GAP           8.0f
#define FULL_VIEW_TOP      HEADER_H
#define FULL_VIEW_H        (CONTENT_BOTTOM - FULL_VIEW_TOP)
#define FULL_FOLLOW_OFFSET (FULL_VIEW_H / 3.0f)
#define DOCUMENT_PADDING   8.0f

/* ui_text() has a 256-byte staging buffer. Leave room for its terminator and
 * keep the duplicate faux-bold draw on exactly the same bounded row. */
#define DISPLAY_ROW_BYTES 240U

#define CLR_WHITE      C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define CLR_PAST       C2D_Color32(0xFF, 0xFF, 0xFF, 0x4D)
#define CLR_GREEN      C2D_Color32(0x1D, 0xB9, 0x54, 0xFF)
#define CLR_FOLLOW_OFF C2D_Color32(0x88, 0x88, 0x88, 0xFF)
#define CLR_PRESS      C2D_Color32(0xFF, 0xFF, 0xFF, 0x18)
#define CLR_BAR_BG     C2D_Color32(0x32, 0x32, 0x32, 0xFF)
#define CLR_STATUS     C2D_Color32(0xFF, 0xFF, 0xFF, 0xA8)
#define CLR_PLAIN      C2D_Color32(0xFF, 0xFF, 0xFF, 0xD8)

#define LOAD_X 36.0f
#define LOAD_Y 143.0f
#define LOAD_W 248.0f
#define LOAD_H 6.0f
#define LOAD_SEGMENT_W 54.0f

static u8 scaled_component(u8 value, float scale, int floor)
{
	int out = floor + (int)((float)value * scale);
	if (out > 255)
		out = 255;
	return (u8)out;
}

static u8 tint_component(u8 value, float white)
{
	return (u8)((float)value + (255.0f - (float)value) * white);
}

static void accent_of(const screen_lyrics_args *a, u8 *r, u8 *g, u8 *b)
{
	if (a->art && a->art->valid) {
		*r = a->art->accent_r;
		*g = a->art->accent_g;
		*b = a->art->accent_b;
	} else {
		*r = 0x20;
		*g = 0x43;
		*b = 0x68;
	}
}

static size_t bounded_length(const char *text)
{
	if (!text)
		return 0;

	size_t length = 0;
	while (length < LYRICS_TEXT_MAX && text[length])
		length++;
	return length;
}

static bool ascii_space(unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static size_t utf8_character_bytes(const char *text, size_t remaining)
{
	const unsigned char lead = (unsigned char)text[0];
	size_t wanted;

	if (lead < 0x80)
		return 1;
	if ((lead & 0xE0) == 0xC0)
		wanted = 2;
	else if ((lead & 0xF0) == 0xE0)
		wanted = 3;
	else if ((lead & 0xF8) == 0xF0)
		wanted = 4;
	else
		return 1;

	if (wanted > remaining)
		return 1;
	for (size_t i = 1; i < wanted; i++) {
		if (((unsigned char)text[i] & 0xC0) != 0x80)
			return 1;
	}
	return wanted;
}

static float prefix_width(C2D_TextBuf buf, const char *text, size_t bytes,
	                      char row[DISPLAY_ROW_BYTES + 1])
{
	memcpy(row, text, bytes);
	row[bytes] = '\0';
	return ui_text_width(buf, row, TY_ALBUM_L);
}

/* Produce one display-safe row and return the source offset of the next row.
 * Width fitting is logarithmic in the number of UTF-8 characters. */
static size_t next_wrapped_row(C2D_TextBuf buf, const char *text, size_t length,
	                          size_t offset, float max_width,
	                          char row[DISPLAY_ROW_BYTES + 1])
{
	while (offset < length && ascii_space((unsigned char)text[offset]))
		offset++;
	if (offset >= length) {
		row[0] = '\0';
		return length;
	}

	size_t boundaries[DISPLAY_ROW_BYTES + 1];
	size_t positions = 1;
	size_t bytes = 0;
	boundaries[0] = 0;
	while (offset + bytes < length) {
		const size_t character = utf8_character_bytes(
		    text + offset + bytes, length - offset - bytes);
		if (bytes + character > DISPLAY_ROW_BYTES)
			break;
		bytes += character;
		boundaries[positions++] = bytes;
	}

	size_t low = 1;
	size_t high = positions - 1;
	size_t fit = 0;
	while (low <= high) {
		const size_t middle = low + (high - low) / 2;
		if (prefix_width(buf, text + offset, boundaries[middle], row) <=
		    max_width) {
			fit = middle;
			low = middle + 1;
		} else {
			high = middle - 1;
		}
	}
	if (fit == 0)
		fit = 1;

	size_t end = boundaries[fit];
	const bool consumed_all = offset + end >= length;
	if (!consumed_all) {
		size_t word_break = 0;
		for (size_t i = 0; i < end; i++) {
			if (ascii_space((unsigned char)text[offset + i]))
				word_break = i;
		}
		if (word_break > 0)
			end = word_break;
	}

	while (end > 0 && ascii_space((unsigned char)text[offset + end - 1]))
		end--;
	memcpy(row, text + offset, end);
	row[end] = '\0';

	size_t next = offset + end;
	while (next < length && ascii_space((unsigned char)text[next]))
		next++;
	/* Invalid or pathological input must still make forward progress. */
	if (next <= offset)
		next = offset + boundaries[fit];
	return next;
}

static unsigned short wrapped_rows(C2D_TextBuf buf, const char *text,
	                               float max_width)
{
	const size_t length = bounded_length(text);
	if (length == 0)
		return 1;

	unsigned int rows = 0;
	size_t offset = 0;
	char row[DISPLAY_ROW_BYTES + 1];
	while (offset < length) {
		offset = next_wrapped_row(buf, text, length, offset, max_width, row);
		rows++;
	}
	return rows > UINT16_MAX ? UINT16_MAX : (unsigned short)rows;
}

void lyrics_layout_free(lyrics_layout *layout)
{
	if (!layout)
		return;
	free(layout->tops);
	free(layout->heights);
	free(layout->rows);
	memset(layout, 0, sizeof *layout);
}

bool lyrics_layout_build(lyrics_layout *layout, C2D_TextBuf buf,
	                     const lyrics_doc *doc)
{
	if (!layout || !doc || doc->count > LYRICS_MAX_LINES)
		return false;

	lyrics_layout next = {0};
	next.count = doc->count;
	if (next.count) {
		next.tops = malloc(next.count * sizeof *next.tops);
		next.heights = malloc(next.count * sizeof *next.heights);
		next.rows = malloc(next.count * sizeof *next.rows);
		if (!next.tops || !next.heights || !next.rows) {
			lyrics_layout_free(&next);
			return false;
		}
	}

	float top = DOCUMENT_PADDING;
	const float text_width = doc->synced ? FULL_TEXT_W : FULL_W;
	for (size_t i = 0; i < next.count; i++) {
		next.rows[i] = wrapped_rows(buf, doc->lines[i].text, text_width);
		next.tops[i] = top;
		next.heights[i] = (float)next.rows[i] * FULL_LEADING;
		top += next.heights[i];
		if (i + 1 < next.count)
			top += FULL_GAP;
	}
	next.document_height = top + DOCUMENT_PADDING;

	lyrics_layout_free(layout);
	*layout = next;
	return true;
}

static float clamp_scroll(float scroll, float max_scroll)
{
	if (scroll < 0.0f)
		return 0.0f;
	if (scroll > max_scroll)
		return max_scroll;
	return scroll;
}

static bool prepared_document(const screen_lyrics_args *a)
{
	return a && a->doc && a->layout && a->layout->count == a->doc->count &&
	       a->layout->count <= LYRICS_MAX_LINES;
}

float screen_lyrics_max_scroll(const screen_lyrics_args *a)
{
	if (!prepared_document(a))
		return 0.0f;
	const float maximum = a->layout->document_height - FULL_VIEW_H;
	return maximum > 0.0f ? maximum : 0.0f;
}

float screen_lyrics_follow_scroll(const screen_lyrics_args *a)
{
	const float maximum = screen_lyrics_max_scroll(a);
	if (!prepared_document(a) || !a->doc->synced || a->highlight < 0 ||
	    (size_t)a->highlight >= a->layout->count)
		return a ? clamp_scroll(a->scroll, maximum) : 0.0f;

	const float target = a->layout->tops[a->highlight] - FULL_FOLLOW_OFFSET;
	return clamp_scroll(target, maximum);
}

static void faux_text(C2D_TextBuf buf, const char *text, float x, float baseline,
	                  type_role role, float max_width, u32 color)
{
	ui_text(buf, text, x, baseline, role, max_width, color);
	ui_text(buf, text, x + 0.75f, baseline, role, max_width - 0.75f, color);
}

static void draw_back_chevron(float cy, u32 color)
{
	C2D_DrawLine(18.0f, cy - 6.0f, color, 12.0f, cy, color, 2.0f, 0.0f);
	C2D_DrawLine(12.0f, cy, color, 18.0f, cy + 6.0f, color, 2.0f, 0.0f);
}

static void draw_header(const screen_lyrics_args *a, u32 background)
{
	const char *title = a->track && a->track[0] ? a->track : "Lyrics";
	const bool unsynced = a->doc && a->doc->count && !a->doc->synced;
	const float follow_width = ui_text_width(a->buf, "FOLLOW", TY_MICRO);
	const float follow_x = BOT_W - 10.0f - follow_width;
	const float follow_hit_x = follow_x - 8.0f;
	float title_right = follow_x - 9.0f;

	C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, BOT_W, HEADER_H, background);
	if (a->pressed_id == LYRICS_BTN_BACK)
		C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 40.0f, HEADER_H, CLR_PRESS);
	if (a->pressed_id == LYRICS_BTN_FOLLOW)
		C2D_DrawRectSolid(follow_hit_x, 0.0f, 0.0f,
		                  BOT_W - follow_hit_x, HEADER_H, CLR_PRESS);

	draw_back_chevron(HEADER_H * 0.5f,
	                  a->pressed_id == LYRICS_BTN_BACK ? CLR_GREEN : CLR_WHITE);
	if (unsynced) {
		const float width = ui_text_width(a->buf, "UNSYNCED", TY_MICRO);
		const float x = follow_x - width - 11.0f;
		ui_text_tracked(a->buf, "UNSYNCED", x,
		                ui_baseline((HEADER_H - ui_px(TY_MICRO)) / 2.0f,
		                            TY_MICRO),
		                TY_MICRO, 0.2f, CLR_STATUS);
		title_right = x - 7.0f;
	}
	ui_text(a->buf, title, 29.0f,
	        ui_baseline((HEADER_H - ui_px(TY_ROW_NAME)) / 2.0f, TY_ROW_NAME),
	        TY_ROW_NAME, title_right - 29.0f, CLR_WHITE);
	ui_text_tracked(a->buf, "FOLLOW", follow_x,
	                ui_baseline((HEADER_H - ui_px(TY_MICRO)) / 2.0f,
	                            TY_MICRO),
	                TY_MICRO, 0.45f,
	                a->follow && a->doc && a->doc->synced ? CLR_GREEN
	                                                       : CLR_FOLLOW_OFF);

	if (a->tb) {
		tb_add(a->tb, 0.0f, 0.0f, 40.0f, HEADER_H, LYRICS_BTN_BACK);
		tb_add(a->tb, follow_hit_x, 0.0f, BOT_W - follow_hit_x, HEADER_H,
		       LYRICS_BTN_FOLLOW);
	}
}

static void draw_clipped_rect(float x, float y, float width, float height,
	                          u32 color)
{
	const float top = y < FULL_VIEW_TOP ? FULL_VIEW_TOP : y;
	const float raw_bottom = y + height;
	const float bottom = raw_bottom > CONTENT_BOTTOM ? CONTENT_BOTTOM : raw_bottom;
	if (bottom > top)
		C2D_DrawRectSolid(x, top, 0.0f, width, bottom - top, color);
}

static void add_line_hit(const screen_lyrics_args *a, float y, float height,
	                     int id)
{
	if (!a->tb)
		return;
	const float top = y < FULL_VIEW_TOP ? FULL_VIEW_TOP : y;
	const float raw_bottom = y + height;
	const float bottom = raw_bottom > CONTENT_BOTTOM ? CONTENT_BOTTOM : raw_bottom;
	if (bottom > top)
		tb_add(a->tb, 0.0f, top, BOT_W, bottom - top, id);
}

static void format_timestamp(uint32_t timestamp_ms, char *out, size_t out_size)
{
	const unsigned long seconds = (unsigned long)timestamp_ms / 1000UL;
	snprintf(out, out_size, "%lu:%02lu", seconds / 60UL, seconds % 60UL);
}

static void draw_scroll_indicator(float scroll, float maximum)
{
	const float thumb_height = 28.0f;
	const float ratio = maximum > 0.0f ? clamp_scroll(scroll, maximum) / maximum
	                                         : 0.0f;
	const float thumb_y = FULL_VIEW_TOP + (FULL_VIEW_H - thumb_height) * ratio;
	C2D_DrawRectSolid(BOT_W - 3.0f, FULL_VIEW_TOP, 0.0f, 3.0f, FULL_VIEW_H,
	                  C2D_Color32(0x00, 0x00, 0x00, 0x30));
	C2D_DrawRectSolid(BOT_W - 3.0f, thumb_y, 0.0f, 3.0f, thumb_height,
	                  C2D_Color32(0xFF, 0xFF, 0xFF, 0x85));
}

static size_t first_visible_line(const lyrics_layout *layout, float document_top)
{
	size_t low = 0;
	size_t high = layout->count;
	while (low < high) {
		const size_t middle = low + (high - low) / 2;
		const float bottom = middle + 1 < layout->count
		                         ? layout->tops[middle + 1]
		                         : layout->tops[middle] + layout->heights[middle];
		if (bottom <= document_top)
			low = middle + 1;
		else
			high = middle;
	}
	return low;
}

static void draw_wrapped_line(const screen_lyrics_args *a, size_t index,
	                          float top, u32 color)
{
	const char *text = a->doc->lines[index].text;
	const size_t length = bounded_length(text);
	const float text_x = a->doc->synced ? FULL_TEXT_X : FULL_X;
	const float text_width = a->doc->synced ? FULL_TEXT_W : FULL_W;
	if (length == 0)
		return;

	size_t offset = 0;
	unsigned int row_number = 0;
	char row[DISPLAY_ROW_BYTES + 1];
	while (offset < length && row_number < a->layout->rows[index]) {
		offset = next_wrapped_row(a->buf, text, length, offset, text_width, row);
		const float row_top = top + (float)row_number * FULL_LEADING;
		if (row_top + ui_px(TY_ALBUM_L) > FULL_VIEW_TOP &&
		    row_top < CONTENT_BOTTOM)
			faux_text(a->buf, row, text_x,
			          ui_baseline(row_top, TY_ALBUM_L), TY_ALBUM_L,
			          text_width, color);
		row_number++;
	}
}

static void draw_document(const screen_lyrics_args *a, u8 ar, u8 ag, u8 ab)
{
	const lyrics_layout *layout = a->layout;
	const bool synced = a->doc->synced;
	const u32 upcoming = C2D_Color32(tint_component(ar, 0.68f),
	                                 tint_component(ag, 0.68f),
	                                 tint_component(ab, 0.68f), 0xFF);
	const float scroll = clamp_scroll(a->scroll, screen_lyrics_max_scroll(a));
	const float document_bottom = scroll + FULL_VIEW_H;
	size_t index = first_visible_line(layout, scroll);

	for (; index < layout->count && layout->tops[index] < document_bottom;
	     index++) {
		const float y = FULL_VIEW_TOP + layout->tops[index] - scroll;
		const float block_height = index + 1 < layout->count
		                               ? layout->tops[index + 1] -
		                                     layout->tops[index]
		                               : layout->heights[index];
		u32 color = CLR_PLAIN;
		if (synced) {
			color = (int)index < a->highlight
			            ? CLR_PAST
			            : ((int)index == a->highlight ? CLR_WHITE : upcoming);
		}

		if (synced && a->pressed_id == LYRICS_LINE0 + (int)index)
			draw_clipped_rect(0.0f, y, BOT_W, block_height, CLR_PRESS);
		if (synced && y + ui_px(TY_ROW_SUB) > FULL_VIEW_TOP &&
		    y < CONTENT_BOTTOM) {
			char timestamp[20];
			format_timestamp(a->doc->lines[index].time_ms, timestamp,
			                 sizeof timestamp);
			const float width = ui_text_width(a->buf, timestamp, TY_ROW_SUB);
			ui_text(a->buf, timestamp, FULL_X + FULL_TIME_W - width,
			        ui_baseline(y +
			                        (FULL_LEADING - ui_px(TY_ROW_SUB)) * 0.5f -
			                        2.0f,
			                    TY_ROW_SUB),
			        TY_ROW_SUB, FULL_TIME_W, color);
		}
		draw_wrapped_line(a, index, y, color);
		if (synced)
			add_line_hit(a, y, block_height, LYRICS_LINE0 + (int)index);
	}

	draw_scroll_indicator(scroll, screen_lyrics_max_scroll(a));
}

static void draw_loading_progress(const screen_lyrics_args *a)
{
	C2D_DrawRectSolid(LOAD_X, LOAD_Y, 0.0f, LOAD_W, LOAD_H, CLR_BAR_BG);
	if (a->loading_total_known && a->loading_total > 0) {
		size_t received = a->loading_received;
		if (received > a->loading_total)
			received = a->loading_total;
		const float ratio = (float)received / (float)a->loading_total;
		C2D_DrawRectSolid(LOAD_X, LOAD_Y, 0.0f, LOAD_W * ratio, LOAD_H,
		                  CLR_GREEN);
	} else {
		const unsigned span = (unsigned)(LOAD_W - LOAD_SEGMENT_W);
		const unsigned cycle = span ? (a->loading_animation_ms / 7) % (span * 2)
		                             : 0;
		const float offset =
		    (float)(cycle <= span ? cycle : span * 2 - cycle);
		C2D_DrawRectSolid(LOAD_X + offset, LOAD_Y, 0.0f, LOAD_SEGMENT_W,
		                  LOAD_H, CLR_GREEN);
	}

	const unsigned received_kb =
	    (unsigned)((a->loading_received + 1023u) / 1024u);
	char amount[40];
	if (a->loading_total_known && a->loading_total > 0) {
		const unsigned total_kb =
		    (unsigned)((a->loading_total + 1023u) / 1024u);
		snprintf(amount, sizeof amount, "%u / %u KB", received_kb, total_kb);
	} else {
		snprintf(amount, sizeof amount, "%u KB", received_kb);
	}
	const float width = ui_text_width(a->buf, amount, TY_ROW_SUB);
	ui_text(a->buf, amount, (BOT_W - width) * 0.5f,
	        ui_baseline(157.0f, TY_ROW_SUB), TY_ROW_SUB, width, CLR_STATUS);
}

static void draw_shell(const screen_lyrics_args *a)
{
	const char *title;
	if (a->loading)
		title = "LOADING LYRICS";
	else if (a->error)
		title = "LYRICS ERROR";
	else
		title = a->status && a->status[0] ? a->status : "NO LYRICS FOUND";

	const float title_width = ui_text_width(a->buf, title, TY_ROW_NAME);
	ui_text_tracked(a->buf, title, (BOT_W - title_width) * 0.5f,
	                ui_baseline(91.0f, TY_ROW_NAME), TY_ROW_NAME, 0.35f,
	                CLR_WHITE);
	if (a->status && a->status[0] && (a->loading || a->error))
		ui_text(a->buf, a->status, 36.0f, ui_baseline(117.0f, TY_ROW_SUB),
		        TY_ROW_SUB, BOT_W - 72.0f, CLR_STATUS);
	if (a->loading)
		draw_loading_progress(a);

	if (a->error) {
		const float x = 92.0f;
		const float y = 145.0f;
		const float width = 136.0f;
		const float height = 34.0f;
		C2D_DrawRectSolid(x, y, 0.0f, width, height,
		                  a->pressed_id == LYRICS_BTN_RETRY ? CLR_PRESS
		                                                     : CLR_BAR_BG);
		const float label_width = ui_text_width(a->buf, "RETRY", TY_ROW_NAME);
		ui_text_tracked(a->buf, "RETRY", x + (width - label_width) * 0.5f,
		                ui_baseline(y + 8.0f, TY_ROW_NAME), TY_ROW_NAME, 0.4f,
		                CLR_WHITE);
		const float hint_width = ui_text_width(a->buf, "X RETRY", TY_MICRO);
		ui_text(a->buf, "X RETRY", (BOT_W - hint_width) * 0.5f,
		        ui_baseline(190.0f, TY_MICRO), TY_MICRO, hint_width,
		        CLR_STATUS);
		if (a->tb)
			tb_add(a->tb, x, y, width, height, LYRICS_BTN_RETRY);
	}
}

void screen_lyrics_bottom_draw(const screen_lyrics_args *a)
{
	if (!a)
		return;

	u8 ar, ag, ab;
	accent_of(a, &ar, &ag, &ab);
	const u32 background = C2D_Color32(scaled_component(ar, 0.72f, 5),
	                                   scaled_component(ag, 0.72f, 5),
	                                   scaled_component(ab, 0.72f, 5), 0xFF);
	const u32 header = C2D_Color32(scaled_component(ar, 0.42f, 3),
	                               scaled_component(ag, 0.42f, 3),
	                               scaled_component(ab, 0.42f, 3), 0xFF);

	C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, BOT_W, BOT_H, background);
	if (!a->loading && !a->error && prepared_document(a) && a->doc->count)
		draw_document(a, ar, ag, ab);
	else
		draw_shell(a);

	ui_progress_bar(a->elapsed_ms, a->duration_ms);
	/* Content is deliberately drawn first; this opaque fixed header is its clip
	 * mask on Citro2D, which has no scissor API. */
	draw_header(a, header);
}
