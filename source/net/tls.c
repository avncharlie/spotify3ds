#include "tls.h"

#include <3ds.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "../testlog.h"
#include "net.h"

/* DER roots embedded by bin2s from the data directory (see Makefile).
 *   DigiCert G2 (RSA) -> api.spotify.com, accounts.spotify.com
 *   DigiCert G3 (ECC) -> i.scdn.co         (album art CDN)
 *   GlobalSign R3     -> mosaic.scdn.co    (generated playlist mosaics)
 *   GTS Root R4       -> lrclib.net         (lyrics provider)
 *
 * All four are required, and the failure mode when one is missing is
 * consistently confusing: the API works while some subset of images silently
 * never loads. Spotify serves different asset classes from different CDNs with
 * different issuers, so a host that has always worked is no guarantee about a
 * new one - mosaic.scdn.co was the first thing to need GlobalSign, and it took
 * a TLS verify error to notice.
 *
 * We ship our own trust store because the console's sslc service caps at TLS
 * 1.1 and Spotify requires 1.2+, so mbedTLS runs in userspace with no system
 * CA bundle behind it. */
extern const unsigned char digicert_g2_der[];
extern const unsigned char digicert_g2_der_end[];
extern const unsigned char digicert_g3_der[];
extern const unsigned char digicert_g3_der_end[];
extern const unsigned char globalsign_r3_der[];
extern const unsigned char globalsign_r3_der_end[];
extern const unsigned char gts_root_r4_der[];
extern const unsigned char gts_root_r4_der_end[];

struct tls_conn {
	int                      fd;
	mbedtls_ssl_context      ssl;
	mbedtls_ssl_config       conf;
	mbedtls_x509_crt         cas;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_entropy_context  entropy;
};

/* ------------------------------------------------------------------ *
 * Entropy
 *
 * This mbedTLS portlib is built with MBEDTLS_NO_PLATFORM_ENTROPY and
 * MBEDTLS_ENTROPY_HARDWARE_ALT, so its only built-in source is
 * mbedtls_hardware_poll() -> sslcGenerateRandomData(). That requires sslcInit()
 * (done in net_init) and works on both hardware and Azahar.
 *
 * PS_GenerateRandomBytes is the other console RNG and is the more natural
 * choice, but Azahar returns 0xD8E007F7 (unavailable) for it. It is therefore
 * registered as an *optional* extra source: contributing when present, and
 * silently contributing nothing when not. Returning ENTROPY_SOURCE_FAILED here
 * would poison the whole accumulator and break seeding even though the sslc
 * source is healthy.
 * ------------------------------------------------------------------ */
static int ps_entropy_poll(void *data, unsigned char *output, size_t len,
                           size_t *olen)
{
	(void)data;

	if (R_FAILED(PS_GenerateRandomBytes(output, len))) {
		*olen = 0; /* no bytes contributed, but not an error */
		return 0;
	}

	*olen = len;
	return 0;
}

/* mbedTLS BIO callbacks over libctru sockets. mbedTLS wants -1 for "would
 * block" mapped onto its own WANT_READ/WANT_WRITE codes so it can retry. */
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	int fd = (int)(intptr_t)ctx;
	int n  = send(fd, buf, len, 0);
	if (n < 0) {
		if (errno == EWOULDBLOCK || errno == EAGAIN)
			return MBEDTLS_ERR_SSL_WANT_WRITE;
		return MBEDTLS_ERR_NET_SEND_FAILED;
	}
	return n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	int fd = (int)(intptr_t)ctx;
	int n  = recv(fd, buf, len, 0);
	if (n < 0) {
		if (errno == EWOULDBLOCK || errno == EAGAIN)
			return MBEDTLS_ERR_SSL_WANT_READ;
		return MBEDTLS_ERR_NET_RECV_FAILED;
	}
	return n;
}

static void describe(int ret, char *out, int outlen)
{
	char buf[128];
	mbedtls_strerror(ret, buf, sizeof buf);
	snprintf(out, outlen, "%s (-0x%04X)", buf, (unsigned)-ret);
}

tls_conn *tls_connect(const char *host, int port, char *err, int errlen)
{
	tls_conn *c = calloc(1, sizeof *c);
	if (!c) {
		snprintf(err, errlen, "oom");
		return NULL;
	}
	c->fd = -1;

	mbedtls_ssl_init(&c->ssl);
	mbedtls_ssl_config_init(&c->conf);
	mbedtls_x509_crt_init(&c->cas);
	mbedtls_ctr_drbg_init(&c->drbg);
	mbedtls_entropy_init(&c->entropy);

	int ret;

	/* Extra RNG source, threshold 0 because it contributes nothing under
	 * emulation. The portlib's own sslc-backed hardware_poll is the source
	 * that actually satisfies the seed. */
	ret = mbedtls_entropy_add_source(&c->entropy, ps_entropy_poll, NULL, 0,
	                                 MBEDTLS_ENTROPY_SOURCE_WEAK);
	if (ret != 0) {
		snprintf(err, errlen, "entropy_add_source");
		goto fail;
	}

	static const char pers[] = "spotify3ds";
	ret = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy,
	                            (const unsigned char *)pers, sizeof pers - 1);
	if (ret != 0) {
		describe(ret, err, errlen);
		goto fail;
	}

	/* --- trust anchors ---------------------------------------------- */
	{
		const struct {
			const unsigned char *begin, *end;
		} roots[] = {
			{digicert_g2_der, digicert_g2_der_end},
			{digicert_g3_der, digicert_g3_der_end},
			{globalsign_r3_der, globalsign_r3_der_end},
			{gts_root_r4_der, gts_root_r4_der_end},
		};

		for (unsigned i = 0; i < sizeof roots / sizeof roots[0]; i++) {
			ret = mbedtls_x509_crt_parse_der(
			    &c->cas, roots[i].begin,
			    (size_t)(roots[i].end - roots[i].begin));
			if (ret != 0) {
				describe(ret, err, errlen);
				goto fail;
			}
		}
	}

	/* --- config ------------------------------------------------------ */
	ret = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
	                                  MBEDTLS_SSL_TRANSPORT_STREAM,
	                                  MBEDTLS_SSL_PRESET_DEFAULT);
	if (ret != 0) {
		describe(ret, err, errlen);
		goto fail;
	}

	/* Pin TLS 1.2: Spotify rejects <1.2, and 1.3 is only experimental in the
	 * packaged mbedTLS 2.28.x. */
	mbedtls_ssl_conf_min_version(&c->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
	                             MBEDTLS_SSL_MINOR_VERSION_3);
	mbedtls_ssl_conf_max_version(&c->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
	                             MBEDTLS_SSL_MINOR_VERSION_3);

	mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&c->conf, &c->cas, NULL);
	mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);

	ret = mbedtls_ssl_setup(&c->ssl, &c->conf);
	if (ret != 0) {
		describe(ret, err, errlen);
		goto fail;
	}

	/* SNI. Spotify serves a wildcard cert per hostname and requires this. */
	ret = mbedtls_ssl_set_hostname(&c->ssl, host);
	if (ret != 0) {
		describe(ret, err, errlen);
		goto fail;
	}

	/* --- transport --------------------------------------------------- */
	c->fd = net_tcp_connect(host, port, err, errlen);
	if (c->fd < 0)
		goto fail;

	mbedtls_ssl_set_bio(&c->ssl, (void *)(intptr_t)c->fd, bio_send, bio_recv,
	                    NULL);

	/* --- handshake ---------------------------------------------------- */
	while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
		if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;

		describe(ret, err, errlen);

		/* Certificate problems are the most likely failure and the most
		 * confusing, so surface the specific verify flags. */
		if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
			uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
			char     why[256];
			mbedtls_x509_crt_verify_info(why, sizeof why, "", flags);
			/* verify_info emits a trailing newline. */
			why[strcspn(why, "\n")] = '\0';
			snprintf(err, errlen, "verify flags=0x%08lX %s",
			         (unsigned long)flags, why);
		}
		goto fail;
	}

	return c;

fail:
	tls_close(c);
	return NULL;
}

const char *tls_ciphersuite(const tls_conn *c)
{
	return mbedtls_ssl_get_ciphersuite(&c->ssl);
}

const char *tls_version(const tls_conn *c)
{
	return mbedtls_ssl_get_version(&c->ssl);
}

bool tls_write(tls_conn *c, const void *buf, size_t len)
{
	const unsigned char *p    = buf;
	size_t               sent = 0;

	while (sent < len) {
		int ret = mbedtls_ssl_write(&c->ssl, p + sent, len - sent);
		if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (ret <= 0)
			return false;
		sent += ret;
	}
	return true;
}

int tls_read(tls_conn *c, void *buf, size_t len)
{
	for (;;) {
		int ret = mbedtls_ssl_read(&c->ssl, buf, len);
		if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		/* Peer closed cleanly: report as EOF, not an error. */
		if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
			return 0;
		return ret;
	}
}

void tls_close(tls_conn *c)
{
	if (!c)
		return;

	if (c->fd >= 0) {
		mbedtls_ssl_close_notify(&c->ssl);
		net_close(c->fd);
	}

	mbedtls_ssl_free(&c->ssl);
	mbedtls_ssl_config_free(&c->conf);
	mbedtls_x509_crt_free(&c->cas);
	mbedtls_ctr_drbg_free(&c->drbg);
	mbedtls_entropy_free(&c->entropy);
	free(c);
}
