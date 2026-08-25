#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "setup/setup_qr.h"

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

int main(void)
{
	const char *client = "0123456789abcdef0123456789abcdef";
	const char *refresh = "AQB-test_refresh-token_123";
	unsigned char payload[512] = {'S', 'P', '3', 'D', 1, 0};
	const size_t client_len = strlen(client);
	const size_t refresh_len = strlen(refresh);
	payload[6] = (unsigned char)client_len;
	payload[7] = (unsigned char)(refresh_len >> 8);
	payload[8] = (unsigned char)refresh_len;
	memcpy(payload + 9, client, client_len);
	memcpy(payload + 9 + client_len, refresh, refresh_len);
	const size_t checksum_at = 9 + client_len + refresh_len;
	const uint32_t crc = crc32_ieee(payload, checksum_at);
	payload[checksum_at] = (unsigned char)(crc >> 24);
	payload[checksum_at + 1] = (unsigned char)(crc >> 16);
	payload[checksum_at + 2] = (unsigned char)(crc >> 8);
	payload[checksum_at + 3] = (unsigned char)crc;

	setup_credentials credentials;
	char err[128];
	assert(setup_qr_parse(payload, checksum_at + 4, &credentials, err,
	                      sizeof err));
	assert(strcmp(credentials.client_id, client) == 0);
	assert(strcmp(credentials.refresh_token, refresh) == 0);
	payload[checksum_at + 3] ^= 1;
	assert(!setup_qr_parse(payload, checksum_at + 4, &credentials, err,
	                       sizeof err));
	puts("setup QR: protocol and checksum passed");
	return 0;
}
