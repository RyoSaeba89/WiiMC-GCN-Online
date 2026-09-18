#include <sys/socket.h>
#include <errno.h>
static inline int net_send(int fd, const void *p, size_t n, int flags)
{ int r = send(fd, p, n, flags); return r < 0 ? -errno : r; }
static inline int net_recv(int fd, void *p, size_t n, int flags)
{ int r = recv(fd, p, n, flags); return r < 0 ? -errno : r; }
