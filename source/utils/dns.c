/*
 * dns.c - minimal DNS resolver for the GameCube
 *
 * Why this file exists: network.h declares net_gethostbyname(), but no library
 * under libogc/lib/cube/ defines it. The gethostbyname() in libogc.a is only a
 * wrapper around that missing symbol, so calling it breaks the link. We
 * therefore implement the strict minimum.
 *
 * The scope is deliberately narrow: A queries, UDP, a single server, no IPv6,
 * no suffix search, no /etc/resolv.conf.
 *
 * A note on parser robustness: names are never expanded. To *skip* a name, a
 * compression pointer (0xC0) terminates it — there is no need to follow it.
 * No loop is therefore possible on a hostile message, and every length is
 * bounded by the number of bytes actually received.
 */

#include <gccore.h>
#include <network.h>
#include <stdio.h>
#include <string.h>

#include "dns.h"

#define DNS_PORT     53
#define DNS_TIMEOUT  2       /* seconds to wait per attempt */
#define DNS_TRIES    3
#define DNS_MAXMSG   512     /* max size of a DNS message over UDP */
#define NAME_MAXLEN  255     /* max length of an encoded name (RFC 1035) */

#define TTL_MIN      30
#define TTL_MAX      3600

static u32 dns_server = 0;

/* ------------------------------------------------------------------ */
/* Cache                                                               */
/* ------------------------------------------------------------------ */

/* Entries live until the next dns_cache_flush(), which is called as soon as a
 * connection fails. This is simpler than per-TTL expiry — which would require
 * a reliable clock — and more responsive: a dynamic-DNS name is resolved again
 * the moment the stream drops, not when the TTL runs out. */

#define CACHE_N 8

static struct {
	char name[128];
	u32  ip;
} cache[CACHE_N];

static int cache_n = 0;

void dns_cache_flush(void)
{
	cache_n = 0;
}

static int cache_get(const char *host, u32 *ip)
{
	int i;
	for (i = 0; i < cache_n; i++) {
		if (!strcmp(cache[i].name, host)) { *ip = cache[i].ip; return 0; }
	}
	return -1;
}

static void cache_put(const char *host, u32 ip)
{
	int slot;

	if ((int)strlen(host) >= (int)sizeof(cache[0].name)) return;
	if (cache_n < CACHE_N) slot = cache_n++;
	else                   slot = 0;      /* table full: overwrite the oldest */

	snprintf(cache[slot].name, sizeof(cache[slot].name), "%s", host);
	cache[slot].ip = ip;
}

/* ------------------------------------------------------------------ */
/* Building the query                                                  */
/* ------------------------------------------------------------------ */

int dns_build_query(u8 *buf, int max, const char *host, u16 id)
{
	const char *p = host;
	int o = 12;

	if (!buf || !host || max < 17) return -1;

	memset(buf, 0, 12);
	buf[0] = (u8)(id >> 8);
	buf[1] = (u8)id;
	buf[2] = 0x01;                       /* RD: recursion requested */
	buf[5] = 0x01;                       /* QDCOUNT = 1 */

	/* The name becomes a sequence of labels prefixed by their length.
	 * A trailing dot takes care of itself. */
	while (*p) {
		const char *dot = strchr(p, '.');
		int lab = dot ? (int)(dot - p) : (int)strlen(p);

		if (lab < 1 || lab > 63)      return -1;   /* empty or oversized label */
		if (o + 1 + lab + 5 > max)    return -1;
		if (o + 1 + lab - 12 > NAME_MAXLEN) return -1;

		buf[o++] = (u8)lab;
		memcpy(buf + o, p, lab);
		o += lab;
		p = dot ? dot + 1 : p + lab;
	}

	if (o == 12) return -1;              /* empty name */

	buf[o++] = 0;                        /* root: end of name */
	buf[o++] = 0; buf[o++] = 1;          /* QTYPE  = A  */
	buf[o++] = 0; buf[o++] = 1;          /* QCLASS = IN */
	return o;
}

/* ------------------------------------------------------------------ */
/* Reading the response                                                */
/* ------------------------------------------------------------------ */

/* Skips an encoded name. Returns the next offset, or -1 if the message is
 * truncated or inconsistent. A compression pointer terminates the name. */
static int skip_name(const u8 *buf, int len, int o)
{
	while (o >= 0 && o < len) {
		u8 c = buf[o];
		if (c == 0)             return o + 1;
		if ((c & 0xC0) == 0xC0) return (o + 2 <= len) ? o + 2 : -1;
		if (c > 63)             return -1;         /* neither short label nor pointer */
		o += 1 + c;
	}
	return -1;
}

int dns_parse_response(const u8 *buf, int len, u16 id, u32 *ip, u32 *ttl)
{
	int qd, an, o = 12, i;

	if (!buf || !ip || len < 12)                    return -1;
	if ((((u16)buf[0] << 8) | buf[1]) != id)        return -1;  /* not our query  */
	if (!(buf[2] & 0x80))                           return -1;  /* not a response */
	if (buf[3] & 0x0F)                              return -1;  /* RCODE != 0     */

	qd = (buf[4] << 8) | buf[5];
	an = (buf[6] << 8) | buf[7];

	for (i = 0; i < qd; i++) {                      /* question section */
		o = skip_name(buf, len, o);
		if (o < 0 || o + 4 > len) return -1;
		o += 4;                                     /* QTYPE + QCLASS */
	}

	/* Sweep every answer until an A record turns up. This walks CNAME chains
	 * naturally: the server returns the whole chain in the same section, the
	 * A record included. */
	for (i = 0; i < an; i++) {
		int type, cls, rdlen;
		u32 t;

		o = skip_name(buf, len, o);
		if (o < 0 || o + 10 > len) return -1;

		type  = (buf[o]     << 8) | buf[o + 1];
		cls   = (buf[o + 2] << 8) | buf[o + 3];
		t     = ((u32)buf[o + 4] << 24) | ((u32)buf[o + 5] << 16) |
		        ((u32)buf[o + 6] <<  8) |  (u32)buf[o + 7];
		rdlen = (buf[o + 8] << 8) | buf[o + 9];
		o += 10;

		if (rdlen < 0 || o + rdlen > len) return -1;

		if (type == 1 && cls == 1 && rdlen == 4) {
			memcpy(ip, buf + o, 4);                 /* already in network order */
			if (ttl) {
				if (t < TTL_MIN) t = TTL_MIN;
				if (t > TTL_MAX) t = TTL_MAX;
				*ttl = t;
			}
			return 0;
		}
		o += rdlen;
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* Querying the server                                                 */
/* ------------------------------------------------------------------ */

void dns_set_server(u32 ip) { dns_server = ip; dns_cache_flush(); }
u32  dns_get_server(void)   { return dns_server; }

static int dns_query(u32 server, const char *host, u32 *ip)
{
	struct sockaddr_in to, from;
	struct timeval tv;
	fd_set  rfds;
	u8      q[DNS_MAXMSG], r[DNS_MAXMSG];
	socklen_t fl;
	static u16 seq = 0;
	u16     id;
	int     qlen, n, try;
	s32     s;

	/* The identifier only serves to discard a late reply on our own socket:
	 * a counter is enough, this is not a security concern on a home LAN. */
	id   = ++seq;
	qlen = dns_build_query(q, sizeof(q), host, id);
	if (qlen < 0) return -1;

	s = net_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s < 0) return -1;

	/* FD_SETSIZE is only 16 in libogc, and FD_SET checks nothing. */
	if (s >= FD_SETSIZE) { net_close(s); return -1; }

	memset(&to, 0, sizeof(to));
	to.sin_family      = AF_INET;
	to.sin_port        = htons(DNS_PORT);
	to.sin_addr.s_addr = server;

	for (try = 0; try < DNS_TRIES; try++) {
		if (net_sendto(s, q, qlen, 0, (struct sockaddr *)&to, sizeof(to)) != qlen)
			continue;

		FD_ZERO(&rfds);
		FD_SET(s, &rfds);
		tv.tv_sec  = DNS_TIMEOUT;
		tv.tv_usec = 0;
		if (net_select(s + 1, &rfds, NULL, NULL, &tv) <= 0) continue;

		fl = sizeof(from);
		n  = net_recvfrom(s, r, sizeof(r), 0, (struct sockaddr *)&from, &fl);
		if (n <= 0) continue;

		if (dns_parse_response(r, n, id, ip, NULL) == 0) {
			net_close(s);
			cache_put(host, *ip);
			return 0;
		}
		/* Unusable answer (wrong id, NXDOMAIN, CNAMEs only): try again, the
		 * server may simply have replied beside the point. */
	}

	net_close(s);
	return -1;
}

int dns_resolve(const char *host, u32 *ip)
{
	struct in_addr a;

	if (!host || !host[0] || !ip) return -1;

	if (inet_aton(host, &a)) {           /* already an IP: nothing to resolve */
		*ip = a.s_addr;
		return 0;
	}
	if (cache_get(host, ip) == 0) return 0;
	if (!dns_server)              return -1;

	return dns_query(dns_server, host, ip);
}

/* ------------------------------------------------------------------ */
/* The shim MPlayer links against                                      */
/* ------------------------------------------------------------------ */

/* stream/network.h turns every gethostbyname() inside MPlayer into a call to
 * this one. MPlayer wants a struct hostent back and reads h_addr_list[0] and
 * h_length out of it; one static entry is enough, because
 * connect2Server_with_af() copies the address out before anything can call
 * again, and MPlayer resolves on a single thread.
 *
 * libogc's own gethostbyname() is a wrapper around net_gethostbyname(), which
 * no cube-side library defines -- which is the whole reason this file is here.
 * See PORTING.md section 3.1. */
struct hostent *wiimc_gethostbyname(const char *name)
{
	static struct hostent he;
	static char *addr_list[2];
	static char *alias_list[1];
	static u32   addr;
	static char  namebuf[128];

	if (dns_resolve(name, &addr) != 0)
		return NULL;

	snprintf(namebuf, sizeof(namebuf), "%s", name);

	addr_list[0]  = (char *)&addr;
	addr_list[1]  = NULL;
	alias_list[0] = NULL;

	he.h_name      = namebuf;
	he.h_aliases   = alias_list;
	he.h_addrtype  = AF_INET;
	he.h_length    = sizeof(addr);
	he.h_addr_list = addr_list;

	return &he;
}
