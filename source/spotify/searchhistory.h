#pragma once

#include <stdbool.h>

/* The queries the user has searched for, most recent first.
 *
 * Retyping on a resistive touchscreen is the slowest input the device has, so
 * a query worth running twice is worth remembering. Long-pressing the search
 * button offers these back.
 *
 * Kept in two scopes, because the two searches look for different things. The
 * library filters collection names it already holds; a collection search scans
 * a playlist's tracks over the network. A track title recalled into the
 * library would match nothing, so offering it there is just noise.
 *
 * All collections share one history rather than getting one each: what is
 * being recalled is a song or an artist, and those recur across playlists.
 */

#define SEARCHHISTORY_MAX       12 /* kept on disk */
#define SEARCHHISTORY_SHOWN      6 /* drawn in the popover */
#define SEARCHHISTORY_QUERY_MAX 64 /* matches the two query buffers in main.c */

/* --- the store, free of any file or console dependency ------------------
 * Split out so the ordering rules can be tested on the host, where the real
 * failure modes of this module live: an off-by-one in the promote-to-front
 * shuffle silently loses somebody's search. */

typedef struct {
	char q[SEARCHHISTORY_MAX][SEARCHHISTORY_QUERY_MAX];
	int  count;
} searchhistory_store;

/* Record `query` as the most recent. Re-running an older one promotes it
 * rather than adding a second copy, and matching ignores case: "Tame" and
 * "tame" are the same recall. The newest spelling wins, so the list shows what
 * was last typed. Empty, whitespace-only and newline-bearing queries are
 * refused - the first two cannot be searched for, and the third would break
 * the one-query-per-line file. At capacity the oldest is dropped. */
void searchhistory_store_push(searchhistory_store *s, const char *query);

/* Forget one entry, closing the gap behind it. */
void searchhistory_store_remove(searchhistory_store *s, int index);

void searchhistory_store_clear(searchhistory_store *s);

/* --- the SD-backed singleton ------------------------------------------- */

typedef enum {
	SEARCHHISTORY_LIBRARY = 0,
	SEARCHHISTORY_TRACKS,
	SEARCHHISTORY_SCOPES,
} searchhistory_scope;

/* Loaded from sdmc:/spotify/searches-<scope>.txt on first use. */
int         searchhistory_count(searchhistory_scope scope);
const char *searchhistory_at(searchhistory_scope scope,
                             int index); /* 0 is newest; NULL out of range */

/* Memory only. Writing costs ~140ms on hardware, so it is deliberately not on
 * the path of a search the user is waiting for. */
void searchhistory_push(searchhistory_scope scope, const char *query);

/* Both write. Only call where a stall is already hidden: after the keyboard
 * applet closes, or as the popover disappears. */
void searchhistory_remove(searchhistory_scope scope, int index);
void searchhistory_clear(searchhistory_scope scope);
void searchhistory_flush(searchhistory_scope scope);
