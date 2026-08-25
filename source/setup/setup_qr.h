#pragma once

#include <stdbool.h>
#include <stddef.h>

#define SETUP_CLIENT_ID_MAX 127
#define SETUP_REFRESH_MAX   255

typedef struct {
	char client_id[SETUP_CLIENT_ID_MAX + 1];
	char refresh_token[SETUP_REFRESH_MAX + 1];
} setup_credentials;

bool setup_qr_parse(const unsigned char *payload, size_t length,
                    setup_credentials *out, char *err, int errlen);
