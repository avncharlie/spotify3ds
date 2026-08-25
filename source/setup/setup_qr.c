#include "setup_qr.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HEADER_SIZE   9U
#define CHECKSUM_SIZE 4U

static uint32_t read_be32(const unsigned char *data)
{
	return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
	       ((uint32_t)data[2] << 8) | data[3];
}

static uint32_t crc32_ieee(const unsigned char *data, size_t length)
{
	uint32_t crc = 0xFFFFFFFFU;
	for (size_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
	}
	return ~crc;
}

static bool safe_text(const unsigned char *data, size_t length)
{
	for (size_t i = 0; i < length; i++)
		if (data[i] < 0x21 || data[i] > 0x7E)
			return false;
	return true;
}

bool setup_qr_parse(const unsigned char *payload, size_t length,
	                setup_credentials *out, char *err, int errlen)
{
	if (!payload || !out || length < HEADER_SIZE + CHECKSUM_SIZE) {
		snprintf(err, errlen, "Setup QR is truncated");
		return false;
	}
	if (memcmp(payload, "SP3D", 4) != 0 || payload[4] != 1 || payload[5] != 0) {
		snprintf(err, errlen, "Unsupported setup QR");
		return false;
	}
	const size_t client_len = payload[6];
	const size_t refresh_len = ((size_t)payload[7] << 8) | payload[8];
	const size_t wanted = HEADER_SIZE + client_len + refresh_len + CHECKSUM_SIZE;
	if (!client_len || client_len > SETUP_CLIENT_ID_MAX || !refresh_len ||
	    refresh_len > SETUP_REFRESH_MAX || wanted != length) {
		snprintf(err, errlen, "Setup QR credential lengths are invalid");
		return false;
	}
	if (read_be32(payload + length - CHECKSUM_SIZE) !=
	    crc32_ieee(payload, length - CHECKSUM_SIZE)) {
		snprintf(err, errlen, "Setup QR checksum failed");
		return false;
	}
	const unsigned char *client = payload + HEADER_SIZE;
	const unsigned char *refresh = client + client_len;
	if (!safe_text(client, client_len) || !safe_text(refresh, refresh_len)) {
		snprintf(err, errlen, "Setup QR contains invalid credentials");
		return false;
	}
	memset(out, 0, sizeof *out);
	memcpy(out->client_id, client, client_len);
	memcpy(out->refresh_token, refresh, refresh_len);
	return true;
}
