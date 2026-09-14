#ifndef DNS_H
#define DNS_H

#include <gccore.h>
#include <network.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal DNS resolver (A records, UDP/53).
 *
 * libogc declares net_gethostbyname() in network.h but never defines it on the
 * GameCube side: only the Wii build implements it. See DOC.md, section 12.
 */

/* Server to query, address in network byte order. 0 = none: only literal IP
 * addresses will resolve. */
void dns_set_server(u32 ip);
u32  dns_get_server(void);

/* "192.168.1.20" or "radio.example.net" -> address in network byte order.
 * Returns 0 on success, -1 otherwise. */
int  dns_resolve(const char *host, u32 *ip);

/* Flushes the cache. Call it whenever a connection fails: this is what makes
 * it possible to follow a dynamic-DNS name whose IP has changed. */
void dns_cache_flush(void);

/* What MPlayer calls: stream/network.h redirects gethostbyname() here.
 * Returns NULL when the name does not resolve. */
struct hostent *wiimc_gethostbyname(const char *name);

/* --- pure logic, exposed for the host test bench --- */

/* Builds an A query. Returns its length, or -1 if the name is invalid. */
int  dns_build_query(u8 *buf, int max, const char *host, u16 id);

/* Extracts the first A address from a response. Returns 0 and fills 'ip'
 * (network byte order) on success, -1 otherwise. 'ttl' may be NULL.
 * Designed to digest a truncated or malformed message without flinching. */
int  dns_parse_response(const u8 *buf, int len, u16 id, u32 *ip, u32 *ttl);

#ifdef __cplusplus
}
#endif

#endif
