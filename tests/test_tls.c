#include "gc_tls.h"
#include "http_client.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

/* Deliberately broken wall clocks, without changing the host's clock. */
static time_t rtc;
time_t time(time_t *out) { if (out) *out = rtc; return rtc; }
void DebugMark(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt); vprintf(fmt, args); va_end(args); putchar('\n');
}
int main(int argc, char **argv)
{
    if (argc == 3) {
        gc_http h;
        char bytes[4096];
        rtc = 946684800;
        gc_tls_init(argv[1]);
        if (gc_http_open(&h, argv[2], "GET", NULL, NULL, -1, -1, 0, NULL, NULL) ||
            gc_http_read(&h, bytes, sizeof(bytes)) <= 0) {
            fprintf(stderr, "Live TLS failed: %s\n", h.error);
            gc_http_close(&h);
            return 1;
        }
        assert(h.status == 200);
        gc_http_close(&h);
        printf("PASS live HTTPS audio, clock 2000: %s\n", argv[2]);
        return 0;
    }
    assert(argc == 5);
    rtc = strtoll(argv[4], NULL, 10);
    gc_tls_init(argv[1]);
    char error[160] = {0}, body[16];
    gc_connection *c = gc_net_open("localhost", atoi(argv[2]), 1, NULL, NULL, error, sizeof(error));
    if (!strcmp(argv[3], "ok")) {
        if (!c) { fprintf(stderr, "Unexpected TLS failure: %s\n", error); return 1; }
        assert(gc_net_read(c, body, sizeof(body)) == 3 && !memcmp(body, "OK\n", 3));
        gc_net_close(c);
    } else {
        if (c) { gc_net_close(c); fprintf(stderr, "Accepted invalid peer\n"); return 1; }
        if (!strcmp(argv[3], "reject")) assert(strstr(error, "certificate rejected"));
        else assert(!strstr(error, "certificate rejected"));
    }
    return 0;
}
