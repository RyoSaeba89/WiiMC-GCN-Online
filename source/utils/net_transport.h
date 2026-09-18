#ifndef WIIMC_NET_TRANSPORT_H
#define WIIMC_NET_TRANSPORT_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef int (*gc_net_cancel_fn)(void *);
typedef struct gc_connection gc_connection;
gc_connection *gc_net_open(const char *host, int port, int tls,
    gc_net_cancel_fn cancel, void *opaque, char *error, size_t error_size);
int gc_net_read(gc_connection *, void *, int);
int gc_net_write(gc_connection *, const void *, int);
void gc_net_close(gc_connection *);
/* Also used by the legacy MPlayer socket path. Leaves the socket nonblocking. */
int gc_net_connect_socket(const char *host, int port, gc_net_cancel_fn, void *);
#ifdef __cplusplus
}
#endif
#endif
