#include "httppool.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "../testlog.h"

/* Requests are serial, so a single connection per active host suffices. Four
 * slots cover the common API/auth/art set; less-used hosts are evicted. */
#define POOL_SLOTS 4

/* Servers close idle keep-alive connections unilaterally. Reusing one that the
 * peer has already dropped costs a failed write plus a reconnect, which is
 * slower than just dialling fresh, so retire them well before that. Spotify's
 * keep-alive timeout is not advertised; 20s is comfortably inside it. */
#define POOL_IDLE_MS 20000

typedef struct {
	char      host[128];
	int       port;
	tls_conn *conn;
	u64       last_used;
} pool_slot;

static pool_slot s_slots[POOL_SLOTS];

static void drop(pool_slot *s)
{
	if (s->conn) {
		tls_close(s->conn);
		s->conn = NULL;
	}
	s->host[0] = '\0';
	s->port    = 0;
}

tls_conn *pool_take(const char *host, int port, bool *reused, char *err,
                    int errlen)
{
	const u64 now = osGetTime();

	if (reused)
		*reused = false;

	for (int i = 0; i < POOL_SLOTS; i++) {
		pool_slot *s = &s_slots[i];
		if (!s->conn)
			continue;

		/* Retire anything that has been sitting too long, whether or not it
		 * matches: a stale entry is only ever a liability. */
		if (now - s->last_used > POOL_IDLE_MS) {
			drop(s);
			continue;
		}

		if (s->port == port && strcmp(s->host, host) == 0) {
			tls_conn *c = s->conn;
			s->conn     = NULL;
			s->host[0]  = '\0';
			if (reused)
				*reused = true;
			return c;
		}
	}

	return tls_connect(host, port, err, errlen);
}

void pool_give(const char *host, int port, tls_conn *c, bool keep)
{
	if (!c)
		return;

	if (!keep) {
		tls_close(c);
		return;
	}

	/* Prefer a free slot; otherwise evict the least recently used one. */
	pool_slot *victim = NULL;
	for (int i = 0; i < POOL_SLOTS; i++) {
		if (!s_slots[i].conn) {
			victim = &s_slots[i];
			break;
		}
		if (!victim || s_slots[i].last_used < victim->last_used)
			victim = &s_slots[i];
	}

	if (victim->conn)
		drop(victim);

	snprintf(victim->host, sizeof victim->host, "%s", host);
	victim->port      = port;
	victim->conn      = c;
	victim->last_used = osGetTime();
}

void pool_clear(void)
{
	for (int i = 0; i < POOL_SLOTS; i++)
		drop(&s_slots[i]);
}
