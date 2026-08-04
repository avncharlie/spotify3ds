#include "json.h"

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
static int subtree_size(const jsmntok_t *toks, int i)
{
	int n = 1;
	if (toks[i].type == JSMN_OBJECT) {
		for (int k = 0; k < toks[i].size; k++) {
			n += 1;                        /* key */
			n += subtree_size(toks, i + n); /* value */
		}
	} else if (toks[i].type == JSMN_ARRAY) {
		for (int k = 0; k < toks[i].size; k++)
			n += subtree_size(toks, i + n);
	}
	return n;
}

/* Walk a dotted path with optional [n] indices. Returns the token index of the
 * addressed value, or -1. */
static int resolve(const char *json, const jsmntok_t *toks, int ntok,
                   const char *path)
{
	(void)ntok;
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
				child += 1 + subtree_size(toks, child + 1);
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

			if (toks[cur].type != JSMN_ARRAY || idx >= toks[cur].size)
				return -1;

			int el = cur + 1;
			for (int k = 0; k < idx; k++)
				el += subtree_size(toks, el);
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
