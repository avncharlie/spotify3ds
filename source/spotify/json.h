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
