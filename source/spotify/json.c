#include "json.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define JSMN_STATIC
#include "jsmn.h"

/* Spotify's currently-playing response is a few hundred tokens; 1024 leaves
 * generous headroom without a heap allocation per poll. */
#define MAX_TOKENS 1024

static bool tok_eq(const char *json, const jsmntok_t *t, const char *s,
                   size_t slen)
{
	size_t tlen = (size_t)(t->end - t->start);
	return t->type == JSMN_STRING && tlen == slen &&
	       strncmp(json + t->start, s, slen) == 0;
}

/* Total number of tokens making up the value at index i (recursively), so we
 * can skip a whole subtree we don't care about. */
static int subtree_size(const jsmntok_t *toks, int ntok, int i)
{
	if (i < 0 || i >= ntok)
		return 0;
	const int end = toks[i].end;
	int next = i + 1;
	while (next < ntok && toks[next].start < end)
		next++;
	return next - i;
}

/* Walk a dotted path with optional [n] indices. Returns the token index of the
 * addressed value, or -1. */
static int resolve(const char *json, const jsmntok_t *toks, int ntok,
                   const char *path)
{
	int cur = 0; /* root */

	const char *p = path;
	while (*p) {
		/* --- key segment ------------------------------------------- */
		const char *seg = p;
		while (*p && *p != '.' && *p != '[')
			p++;
		size_t seglen = (size_t)(p - seg);

		if (seglen) {
			if (toks[cur].type != JSMN_OBJECT)
				return -1;

			int  child = cur + 1;
			bool found = false;
			for (int k = 0; k < toks[cur].size; k++) {
				if (tok_eq(json, &toks[child], seg, seglen)) {
					cur   = child + 1; /* the value */
					found = true;
					break;
				}
				child += 1 + subtree_size(toks, ntok, child + 1);
			}
			if (!found)
				return -1;
		}

		/* --- optional array indices -------------------------------- */
		while (*p == '[') {
			int idx = atoi(p + 1);
			while (*p && *p != ']')
				p++;
			if (*p == ']')
				p++;

			if (toks[cur].type != JSMN_ARRAY || idx < 0 || idx >= toks[cur].size)
				return -1;

			int el = cur + 1;
			for (int k = 0; k < idx; k++)
				el += subtree_size(toks, ntok, el);
			cur = el;
		}

		if (*p == '.')
			p++;
	}

	return cur;
}

/* Decode the JSON string escapes Spotify actually emits in track metadata. */
static void unescape(const char *src, size_t n, char *out, size_t outlen)
{
	size_t o = 0;
	for (size_t i = 0; i < n && o + 1 < outlen; i++) {
		if (src[i] != '\\' || i + 1 >= n) {
			out[o++] = src[i];
			continue;
		}
		char c = src[++i];
		switch (c) {
			case 'n': out[o++] = '\n'; break;
			case 't': out[o++] = '\t'; break;
			case 'r': out[o++] = '\r'; break;
			case 'b': out[o++] = '\b'; break;
			case 'f': out[o++] = '\f'; break;
			case '"': out[o++] = '"'; break;
			case '\\': out[o++] = '\\'; break;
			case '/': out[o++] = '/'; break;
			case 'u': {
				/* \uXXXX -> UTF-8. Track titles routinely contain typographic
				 * punctuation (e.g. ’), so this must be handled. */
				if (i + 4 >= n)
					break;
				char hex[5] = {src[i + 1], src[i + 2], src[i + 3], src[i + 4], 0};
				unsigned cp = (unsigned)strtoul(hex, NULL, 16);
				i += 4;
				if (cp < 0x80) {
					out[o++] = (char)cp;
				} else if (cp < 0x800) {
					if (o + 2 >= outlen) break;
					out[o++] = (char)(0xC0 | (cp >> 6));
					out[o++] = (char)(0x80 | (cp & 0x3F));
				} else {
					if (o + 3 >= outlen) break;
					out[o++] = (char)(0xE0 | (cp >> 12));
					out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
					out[o++] = (char)(0x80 | (cp & 0x3F));
				}
				break;
			}
			default: out[o++] = c; break;
		}
	}
	out[o] = '\0';
}

static int parse_all(const char *json, size_t len, jsmntok_t *toks)
{
	jsmn_parser p;
	jsmn_init(&p);
	return jsmn_parse(&p, json, len, toks, MAX_TOKENS);
}

/* Shared by both the one-shot accessors and the parsed-document ones. */
static bool extract_str(const char *json, const jsmntok_t *toks, int n,
                        const char *path, char *out, size_t outlen)
{
	const int i = resolve(json, toks, n, path);
	if (i < 0)
		return false;
	if (toks[i].type != JSMN_STRING && toks[i].type != JSMN_PRIMITIVE)
		return false;

	unescape(json + toks[i].start, (size_t)(toks[i].end - toks[i].start), out,
	         outlen);
	return true;
}

bool json_get_str(const char *json, size_t len, const char *path, char *out,
                  size_t outlen)
{
	static jsmntok_t toks[MAX_TOKENS];

	int n = parse_all(json, len, toks);
	if (n < 1)
		return false;

	return extract_str(json, toks, n, path, out, outlen);
}

/* --- parse once, read many ---------------------------------------------- */

struct json_doc {
	const char *json;
	jsmntok_t  *toks;
	int         n;
};

json_doc *json_doc_parse(const char *json, size_t len, int *needed)
{
	if (needed)
		*needed = 0;

	if (!json || !len)
		return NULL;

	/* One token per ~11 bytes is generous for Spotify's payloads; cap it so a
	 * surprising body cannot ask for an unbounded allocation.
	 *
	 * The ceiling is sized for the largest response we ask for: recently-played
	 * at limit=50 is ~147KB and 6830 tokens (measured), and that endpoint
	 * ignores `fields=`, so it cannot be trimmed server-side. 32768 tokens is
	 * 512KB transient on a device with 32MB - cheap next to the headroom it
	 * buys for a heavier listening history than the one it was measured on. */
	int cap = (int)(len / 8) + 64;
	if (cap > 32768)
		cap = 32768;

	jsmntok_t *toks = malloc((size_t)cap * sizeof *toks);
	if (!toks)
		return NULL;

	jsmn_parser p;
	jsmn_init(&p);
	const int n = jsmn_parse(&p, json, len, toks, (unsigned)cap);

	if (n < 1) {
		/* NOMEM is the interesting one: the document is well-formed but larger
		 * than the pool. Report the cap so the caller can say by how much. */
		if (needed)
			*needed = (n == JSMN_ERROR_NOMEM) ? cap : n;
		free(toks);
		return NULL;
	}

	json_doc *d = malloc(sizeof *d);
	if (!d) {
		free(toks);
		return NULL;
	}
	d->json = json;
	d->toks = toks;
	d->n    = n;
	if (needed)
		*needed = n;
	return d;
}

int json_doc_tokens(const json_doc *d)
{
	return d ? d->n : 0;
}

void json_doc_free(json_doc *d)
{
	if (!d)
		return;
	free(d->toks);
	free(d);
}

bool json_doc_str(const json_doc *d, const char *path, char *out, size_t outlen)
{
	if (!d)
		return false;
	return extract_str(d->json, d->toks, d->n, path, out, outlen);
}

bool json_doc_int(const json_doc *d, const char *path, long *out)
{
	char buf[32];
	if (!json_doc_str(d, path, buf, sizeof buf))
		return false;
	if (buf[0] == 'n')
		return false;

	char     *end;
	const long v = strtol(buf, &end, 10);
	if (end == buf)
		return false;
	*out = v;
	return true;
}

bool json_doc_bool(const json_doc *d, const char *path, bool *out)
{
	char buf[16];
	if (!json_doc_str(d, path, buf, sizeof buf))
		return false;
	if (strcmp(buf, "true") == 0) {
		*out = true;
		return true;
	}
	if (strcmp(buf, "false") == 0) {
		*out = false;
		return true;
	}
	return false;
}

int json_doc_array_size(const json_doc *d, const char *path)
{
	if (!d)
		return -1;
	const int i = resolve(d->json, d->toks, d->n, path);
	return i >= 0 && d->toks[i].type == JSMN_ARRAY ? d->toks[i].size : -1;
}

bool json_doc_is_null(const json_doc *d, const char *path)
{
	if (!d)
		return false;
	const int i = resolve(d->json, d->toks, d->n, path);
	return i >= 0 && d->toks[i].type == JSMN_PRIMITIVE &&
	       d->toks[i].end - d->toks[i].start == 4 &&
	       strncmp(d->json + d->toks[i].start, "null", 4) == 0;
}

bool json_doc_is_string(const json_doc *d, const char *path)
{
	if (!d)
		return false;
	const int i = resolve(d->json, d->toks, d->n, path);
	return i >= 0 && d->toks[i].type == JSMN_STRING;
}

bool json_doc_is_nonempty_string(const json_doc *d, const char *path)
{
	if (!d)
		return false;
	const int i = resolve(d->json, d->toks, d->n, path);
	return i >= 0 && d->toks[i].type == JSMN_STRING &&
	       d->toks[i].end > d->toks[i].start;
}

bool json_doc_is_object(const json_doc *d, const char *path)
{
	if (!d)
		return false;
	const int i = resolve(d->json, d->toks, d->n, path);
	return i >= 0 && d->toks[i].type == JSMN_OBJECT;
}

static bool hex4(const char *s, unsigned *out)
{
	unsigned value = 0;
	for (int i = 0; i < 4; i++) {
		const unsigned char c = (unsigned char)s[i];
		unsigned digit;
		if (c >= '0' && c <= '9')
			digit = c - '0';
		else if (c >= 'a' && c <= 'f')
			digit = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			digit = c - 'A' + 10;
		else
			return false;
		value = value * 16 + digit;
	}
	*out = value;
	return true;
}

static bool decoded_size(const char *src, size_t len, size_t max_bytes,
	                     size_t *size)
{
	size_t total = 0;
	for (size_t i = 0; i < len; i++) {
		size_t add = 1;
		if (src[i] == '\\') {
			if (++i >= len)
				return false;
			if (src[i] == 'u') {
				unsigned cp;
				if (i + 4 >= len || !hex4(src + i + 1, &cp))
					return false;
				i += 4;
				if (cp >= 0xD800 && cp <= 0xDBFF) {
					unsigned low;
					if (i + 6 >= len || src[i + 1] != '\\' ||
					    src[i + 2] != 'u' || !hex4(src + i + 3, &low) ||
					    low < 0xDC00 || low > 0xDFFF)
						return false;
					i += 6;
					cp = 0x10000 + ((cp - 0xD800) << 10) +
					     (low - 0xDC00);
				} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
					return false;
				}
				if (cp == 0)
					return false;
				add = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
			}
		}
		if (total > max_bytes || add > max_bytes - total)
			return false;
		total += add;
	}
	*size = total;
	return true;
}

static void put_utf8(char *out, size_t *pos, unsigned cp)
{
	if (cp < 0x80) {
		out[(*pos)++] = (char)cp;
	} else if (cp < 0x800) {
		out[(*pos)++] = (char)(0xC0 | (cp >> 6));
		out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		out[(*pos)++] = (char)(0xE0 | (cp >> 12));
		out[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
	} else {
		out[(*pos)++] = (char)(0xF0 | (cp >> 18));
		out[(*pos)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
		out[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
	}
}

static void decode_alloc_string(const char *src, size_t len, char *out)
{
	size_t pos = 0;
	for (size_t i = 0; i < len; i++) {
		if (src[i] != '\\') {
			out[pos++] = src[i];
			continue;
		}

		const char escaped = src[++i];
		switch (escaped) {
			case 'n': out[pos++] = '\n'; break;
			case 't': out[pos++] = '\t'; break;
			case 'r': out[pos++] = '\r'; break;
			case 'b': out[pos++] = '\b'; break;
			case 'f': out[pos++] = '\f'; break;
			case '"': out[pos++] = '"'; break;
			case '\\': out[pos++] = '\\'; break;
			case '/': out[pos++] = '/'; break;
			case 'u': {
				unsigned cp;
				(void)hex4(src + i + 1, &cp);
				i += 4;
				if (cp >= 0xD800 && cp <= 0xDBFF) {
					unsigned low;
					(void)hex4(src + i + 3, &low);
					i += 6;
					cp = 0x10000 + ((cp - 0xD800) << 10) +
					     (low - 0xDC00);
				}
				put_utf8(out, &pos, cp);
				break;
			}
			default: out[pos++] = escaped; break;
		}
	}
	out[pos] = '\0';
}

json_alloc_result json_doc_str_alloc(const json_doc *d, const char *path,
	                                 size_t max_bytes, char **out,
	                                 size_t *outlen)
{
	if (out)
		*out = NULL;
	if (outlen)
		*outlen = 0;
	if (!d || !path || !out)
		return JSON_ALLOC_INVALID;

	const int i = resolve(d->json, d->toks, d->n, path);
	if (i < 0 || (d->toks[i].type == JSMN_PRIMITIVE &&
	              d->toks[i].end - d->toks[i].start == 4 &&
	              strncmp(d->json + d->toks[i].start, "null", 4) == 0))
		return JSON_ALLOC_ABSENT;
	if (d->toks[i].type != JSMN_STRING)
		return JSON_ALLOC_INVALID;

	const char *src = d->json + d->toks[i].start;
	const size_t raw_len = (size_t)(d->toks[i].end - d->toks[i].start);
	size_t decoded_len = 0;
	if (!decoded_size(src, raw_len, max_bytes, &decoded_len)) {
		/* jsmn has already validated escapes, so a lone surrogate is invalid;
		 * otherwise failure here means the decoded value crossed the cap. */
		size_t unlimited_len = 0;
		if (decoded_size(src, raw_len, SIZE_MAX, &unlimited_len))
			return JSON_ALLOC_TOO_LARGE;
		return JSON_ALLOC_INVALID;
	}

	char *value = malloc(decoded_len + 1);
	if (!value)
		return JSON_ALLOC_OOM;
	decode_alloc_string(src, raw_len, value);
	*out = value;
	if (outlen)
		*outlen = decoded_len;
	return JSON_ALLOC_OK;
}

bool json_get_int(const char *json, size_t len, const char *path, long *out)
{
	char buf[32];
	if (!json_get_str(json, len, path, buf, sizeof buf))
		return false;
	if (buf[0] == 'n') /* null */
		return false;

	char *end;
	long  v = strtol(buf, &end, 10);
	if (end == buf)
		return false;

	*out = v;
	return true;
}

bool json_get_bool(const char *json, size_t len, const char *path, bool *out)
{
	char buf[16];
	if (!json_get_str(json, len, path, buf, sizeof buf))
		return false;

	if (strcmp(buf, "true") == 0) {
		*out = true;
		return true;
	}
	if (strcmp(buf, "false") == 0) {
		*out = false;
		return true;
	}
	return false;
}
