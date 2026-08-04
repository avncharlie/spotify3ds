#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Minimal JSON field extraction over jsmn.
 *
 * Only what the player response needs, addressed by dotted path with array
 * indices, e.g.:
 *   "item.name"
 *   "item.artists[0].name"
 *   "item.album.images[0].url"
 *
 * A full DOM would be overkill: we read about eight fields from a response
 * that is otherwise discarded.
 */

/* Copy a string/primitive value into out. Returns false if absent. */
bool json_get_str(const char *json, size_t len, const char *path, char *out,
                  size_t outlen);

/* Parse an integer value. Returns false if absent or not a number. */
bool json_get_int(const char *json, size_t len, const char *path, long *out);

/* Parse a boolean value. Returns false if absent or not true/false. */
bool json_get_bool(const char *json, size_t len, const char *path, bool *out);

/* --- parse once, read many ------------------------------------------------
 *
 * The accessors above re-tokenise the whole document on every call, which is
 * fine for the eight fields of a player response but not for a list: four
 * recently-played items need ~16 lookups over a 13KB body.
 *
 * Just as important, they cannot report *why* they failed. jsmn returns
 * JSMN_ERROR_NOMEM when the document needs more tokens than the fixed pool,
 * and json_get_str turns that into a plain `false` - indistinguishable from an
 * absent field. A response one item too large would silently look like an
 * empty list. json_doc_parse surfaces it.
 */
typedef struct json_doc json_doc;

/* Tokenise `json` into a reusable document. Returns NULL if it does not fit,
 * writing the token count needed into *needed when non-NULL so the caller can
 * log how far over the limit it was. The document borrows `json`; it must stay
 * alive and unmodified until json_doc_free. */
json_doc *json_doc_parse(const char *json, size_t len, int *needed);

/* Tokens actually used, for logging headroom. */
int json_doc_tokens(const json_doc *d);

void json_doc_free(json_doc *d);

/* Same path syntax as above, against an already-parsed document. */
bool json_doc_str(const json_doc *d, const char *path, char *out,
                  size_t outlen);
bool json_doc_int(const json_doc *d, const char *path, long *out);
bool json_doc_bool(const json_doc *d, const char *path, bool *out);

/* Number of direct elements in an array, or -1 when the path is absent or is
 * not an array. This is important for Spotify pages containing null entries:
 * callers must not mistake a missing field in the middle for end-of-list. */
int json_doc_array_size(const json_doc *d, const char *path);
bool json_doc_is_null(const json_doc *d, const char *path);
