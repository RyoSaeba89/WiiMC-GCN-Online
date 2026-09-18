#ifndef WIIMC_GC_TLS_H
#define WIIMC_GC_TLS_H
#include "net_transport.h"
#ifdef __cplusplus
extern "C" {
#endif
void gc_tls_init(const char *app_path);
void *gc_tls_open(int fd, const char *host, gc_net_cancel_fn, void *, char *, size_t);
int gc_tls_io(void *session, void *buffer, int length, int writing);
void gc_tls_close(void *session);
#ifdef __cplusplus
}
#endif
#endif
