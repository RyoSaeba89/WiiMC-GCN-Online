/* Bounded socket I/O. libogc2 returns negative errno values and does not
 * implement SO_RCVTIMEO. Never rely on either newlib's or IOS's flag values. */
#include "net_transport.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#ifdef GEKKO
#include <gccore.h>
#include <network.h>
#include <ogc/lwp_watchdog.h>
#include "dns.h"
#include "debuglog.h"
#else
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <netdb.h>
#include <arpa/inet.h>
#define DebugMark(...) ((void)0)
#define net_close close
#define net_select select
#define net_ioctl ioctl
#define net_fcntl fcntl
#define net_socket socket
static int net_connect(int fd, const struct sockaddr *a, socklen_t n)
{ int r = connect(fd, a, n); return r < 0 ? -errno : r; }
static int net_recv(int fd, void *p, int n, int flags)
{ int r = recv(fd, p, n, flags); return r < 0 ? -errno : r; }
static int net_send(int fd, const void *p, int n, int flags)
{ int r = send(fd, p, n, flags); return r < 0 ? -errno : r; }
#endif
#ifdef WANT_TLS
#include "gc_tls.h"
#endif

struct gc_connection {
    int fd;
    void *tls;
    gc_net_cancel_fn cancel;
    void *opaque;
};

static uint64_t now_ms(void)
{
#ifdef GEKKO
    return ticks_to_millisecs(gettime());
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

static int cancelled(gc_net_cancel_fn fn, void *opaque)
{ return fn && fn(opaque); }

int gc_net_connect_socket(const char *host, int port, gc_net_cancel_fn cancel, void *opaque)
{
    struct sockaddr_in addr;
    int fd, ret;
    unsigned long nonblock = 1;
    uint64_t deadline;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (port < 1 || port > 65535) return -EINVAL;
#ifdef GEKKO
    addr.sin_len = sizeof(addr);
    if (dns_resolve(host, &addr.sin_addr.s_addr) || !addr.sin_addr.s_addr) return -EHOSTUNREACH;
#else
    struct hostent *hp = gethostbyname(host);
    if (!hp || hp->h_length != 4) return -EHOSTUNREACH;
    memcpy(&addr.sin_addr, hp->h_addr_list[0], 4);
#endif
    if (cancelled(cancel, opaque)) return -ECANCELED;
    fd = net_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return fd;
    ret = net_ioctl(fd, FIONBIO, &nonblock);
    DebugMark("net: FIONBIO = %d, flags = %d", ret, net_fcntl(fd, F_GETFL, 0));
    if (ret < 0) { net_close(fd); return ret; }
    deadline = now_ms() + 8000;
    do {
        ret = net_connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        if (ret == 0 || ret == -EISCONN) {
            DebugMark("net: connected on socket %d", fd);
            return fd;
        }
        if (ret != -EINPROGRESS && ret != -EALREADY && ret != -EAGAIN && ret != -EWOULDBLOCK)
            break;
        if (cancelled(cancel, opaque)) { ret = -ECANCELED; break; }
        if (now_ms() >= deadline) { ret = -ETIMEDOUT; break; }
        usleep(20000);
    } while (1);
    DebugMark("net: connect failed (%d)", ret);
    net_close(fd);
#ifdef GEKKO
    dns_cache_flush();
#endif
    return ret;
}

gc_connection *gc_net_open(const char *host, int port, int tls,
    gc_net_cancel_fn cancel, void *opaque, char *error, size_t size)
{
    gc_connection *c = calloc(1, sizeof(*c));
    if (!c) { snprintf(error, size, "Out of memory"); return NULL; }
    c->cancel = cancel;
    c->opaque = opaque;
    c->fd = gc_net_connect_socket(host, port, cancel, opaque);
    if (c->fd < 0) {
        snprintf(error, size, "Connection failed (%d)", c->fd);
        free(c);
        return NULL;
    }
    if (tls) {
#ifdef WANT_TLS
        c->tls = gc_tls_open(c->fd, host, cancel, opaque, error, size);
        if (!c->tls) { gc_net_close(c); return NULL; }
#else
        snprintf(error, size, "HTTPS is disabled in this build");
        gc_net_close(c);
        return NULL;
#endif
    }
    return c;
}

static int transfer(gc_connection *c, void *buf, int len, int writing)
{
    uint64_t deadline = now_ms() + 15000;
    if (len <= 0) return 0;
    while (!cancelled(c->cancel, c->opaque)) {
        int r;
#ifdef WANT_TLS
        if (c->tls) r = gc_tls_io(c->tls, buf, len, writing);
        else
#endif
        r = writing ? net_send(c->fd, buf, len, 0) : net_recv(c->fd, buf, len, 0);
        if (r >= 0) return r;
        if (r != -EAGAIN && r != -EWOULDBLOCK && r != -EINTR) return r;
        if (now_ms() >= deadline) return -ETIMEDOUT;
        /* Nonblocking reads also drain mbedTLS's already decrypted bytes.
         * Sleeping here avoids assuming that readability implies writability
         * during a TLS exchange, or that TLS always needs another TCP packet. */
        usleep(10000);
    }
    return -ECANCELED;
}
int gc_net_read(gc_connection *c, void *p, int n) { return transfer(c, p, n, 0); }
int gc_net_write(gc_connection *c, const void *p, int n) { return transfer(c, (void *)p, n, 1); }
void gc_net_close(gc_connection *c)
{
    if (!c) return;
#ifdef WANT_TLS
    if (c->tls) gc_tls_close(c->tls);
#endif
    if (c->fd >= 0) net_close(c->fd);
    free(c);
}
