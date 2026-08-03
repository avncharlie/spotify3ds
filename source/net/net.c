#include "net.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <malloc.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../testlog.h"

/* soc requires a page-aligned buffer it uses for its own bookkeeping. */
#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000

static u32 *s_soc_buf;
static bool s_ready;
static bool s_sslc_up;

bool net_init(char *err, int errlen)
{
	if (s_ready)
		return true;

	Result rc = acInit();
	if (R_FAILED(rc)) {
		/* Non-fatal: Azahar does not always expose a meaningful AC state, and
		 * soc works regardless. Note it and continue. */
		tl_log("acInit failed rc=0x%08lX (continuing)", rc);
	}

	/* Deliberately NOT calling acWaitInternetConnection(): it can block
	 * forever under emulation. soc:U works without it. */

	s_soc_buf = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
	if (!s_soc_buf) {
		if (err)
			snprintf(err, errlen, "memalign %d failed", SOC_BUFFERSIZE);
		return false;
	}

	rc = socInit(s_soc_buf, SOC_BUFFERSIZE);
	if (R_FAILED(rc)) {
		if (err)
			snprintf(err, errlen, "socInit rc=0x%08lX", rc);
		free(s_soc_buf);
		s_soc_buf = NULL;
		return false;
	}

	/* We do TLS ourselves via mbedTLS (sslc caps at TLS 1.1), but the packaged
	 * mbedcrypto's mbedtls_hardware_poll() still routes through
	 * sslcGenerateRandomData(), so the service has to be up or seeding fails.
	 * Non-fatal: tls.c also registers PS_GenerateRandomBytes as a strong
	 * entropy source, which is sufficient on its own. */
	rc = sslcInit(0);
	if (R_FAILED(rc))
		tl_log("sslcInit failed rc=0x%08lX (PS entropy still available)", rc);
	else
		s_sslc_up = true;

	s_ready = true;
	return true;
}

void net_exit(void)
{
	if (!s_ready)
		return;
	if (s_sslc_up) {
		sslcExit();
		s_sslc_up = false;
	}
	socExit();
	acExit();
	/* The soc buffer is owned by the service until socExit returns; only then
	 * is it safe to release. */
	free(s_soc_buf);
	s_soc_buf = NULL;
	s_ready = false;
}

bool net_resolve(const char *host, char *out_ip, int outlen)
{
	struct hostent *he = gethostbyname(host);
	if (!he || !he->h_addr_list || !he->h_addr_list[0])
		return false;

	struct in_addr addr;
	memcpy(&addr, he->h_addr_list[0], sizeof addr);

	const char *s = inet_ntoa(addr);
	if (!s)
		return false;

	snprintf(out_ip, outlen, "%s", s);
	return true;
}

int net_tcp_connect(const char *host, int port, char *err, int errlen)
{
	char ip[64];
	if (!net_resolve(host, ip, sizeof ip)) {
		if (err)
			snprintf(err, errlen, "resolve %s failed", host);
		return -1;
	}

	int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) {
		if (err)
			snprintf(err, errlen, "socket errno=%d", errno);
		return -1;
	}

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family      = AF_INET;
	sa.sin_port        = htons(port);
	sa.sin_addr.s_addr = inet_addr(ip);

	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		if (err)
			snprintf(err, errlen, "connect %s:%d errno=%d", ip, port, errno);
		closesocket(fd);
		return -1;
	}

	return fd;
}

void net_close(int fd)
{
	if (fd >= 0)
		closesocket(fd);
}
