#pragma once

#include <stdbool.h>

/* Load client_id + refresh_token from sdmc:/spotify/creds.cfg.
 * Returns false if the file is missing or malformed. */
bool auth_load(char *err, int errlen);

/* Current access token, refreshing if absent or near expiry.
 * Returns NULL on failure. */
const char *auth_token(char *err, int errlen);

/* Force a refresh regardless of expiry. Used after a 401. */
bool auth_refresh(char *err, int errlen);
