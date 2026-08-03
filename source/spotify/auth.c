#include "auth.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../net/http.h"
#include "../testlog.h"
#include "json.h"

#define CREDS_PATH "sdmc:/spotify/creds.cfg"
#define ACCOUNTS_HOST "accounts.spotify.com"
#define TOKEN_PATH "/api/token"

/* Refresh this long before actual expiry so a poll never races the deadline. */
#define EXPIRY_MARGIN_S 60

static char s_client_id[128];
static char s_refresh[256];
static char s_access[512];
static u64  s_expires_at; /* osGetTime() ms; 0 = no token yet */

/* Strip trailing CR/LF/space so a CRLF-saved creds.cfg still works. */
static void rstrip(char *s)
{
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' ||
	             s[n - 1] == '\t'))
		s[--n] = '\0';
}

bool auth_load(char *err, int errlen)
{
	FILE *f = fopen(CREDS_PATH, "r");
	if (!f) {
		snprintf(err, errlen, "cannot open %s", CREDS_PATH);
		return false;
	}

	char line[512];
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#')
			continue;
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = '\0';
		char *key = line, *val = eq + 1;
		rstrip(val);

		if (strcmp(key, "client_id") == 0)
			snprintf(s_client_id, sizeof s_client_id, "%s", val);
		else if (strcmp(key, "refresh_token") == 0)
			snprintf(s_refresh, sizeof s_refresh, "%s", val);
	}
	fclose(f);

	if (!s_client_id[0] || !s_refresh[0]) {
		snprintf(err, errlen, "creds.cfg missing client_id or refresh_token");
		return false;
	}
	return true;
}

/* Spotify may hand back a new refresh token; persisting it matters because
 * otherwise auth silently breaks days later. */
static void persist_refresh(const char *new_refresh)
{
	if (!new_refresh || !new_refresh[0] ||
	    strcmp(new_refresh, s_refresh) == 0)
		return;

	snprintf(s_refresh, sizeof s_refresh, "%s", new_refresh);

	FILE *f = fopen(CREDS_PATH, "w");
	if (!f) {
		tl_log("WARN: refresh token rotated but creds.cfg is not writable");
		return;
	}
	fprintf(f, "# spotify3ds credentials - DO NOT COMMIT\n");
	fprintf(f, "client_id=%s\n", s_client_id);
	fprintf(f, "refresh_token=%s\n", s_refresh);
	fclose(f);
	tl_log("refresh token rotated, creds.cfg updated");
}

bool auth_refresh(char *err, int errlen)
{
	if (!s_client_id[0] || !s_refresh[0]) {
		snprintf(err, errlen, "credentials not loaded");
		return false;
	}

	char body[512];
	snprintf(body, sizeof body,
	         "grant_type=refresh_token&refresh_token=%s&client_id=%s",
	         s_refresh, s_client_id);

	http_response r;
	if (!http_request(ACCOUNTS_HOST, "POST", TOKEN_PATH, NULL,
	                  "application/x-www-form-urlencoded", body, &r, err,
	                  errlen))
		return false;

	if (r.status != 200) {
		/* Surface Spotify's own error text: "invalid_grant" here almost always
		 * means the refresh token was revoked. */
		char msg[128] = "";
		if (r.body)
			json_get_str(r.body, r.body_len, "error", msg, sizeof msg);
		snprintf(err, errlen, "token http %d %s", r.status, msg);
		http_free(&r);
		return false;
	}

	char token[512];
	if (!r.body || !json_get_str(r.body, r.body_len, "access_token", token,
	                             sizeof token)) {
		snprintf(err, errlen, "no access_token in response");
		http_free(&r);
		return false;
	}

	long expires = 3600;
	json_get_int(r.body, r.body_len, "expires_in", &expires);

	char rotated[256] = "";
	if (json_get_str(r.body, r.body_len, "refresh_token", rotated,
	                 sizeof rotated))
		persist_refresh(rotated);

	http_free(&r);

	snprintf(s_access, sizeof s_access, "%s", token);
	s_expires_at = osGetTime() + (u64)(expires - EXPIRY_MARGIN_S) * 1000;

	tl_log("access token acquired, expires_in=%ld", expires);
	return true;
}

const char *auth_token(char *err, int errlen)
{
	if (s_access[0] && osGetTime() < s_expires_at)
		return s_access;

	if (!auth_refresh(err, errlen))
		return NULL;

	return s_access;
}
