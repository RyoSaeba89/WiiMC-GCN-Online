/* TLS 1.2 client. Certificate chain and hostname are mandatory. Certificate
 * dates are not checked because a GameCube with a flat RTC battery has no
 * trustworthy wall clock. This matches GCRadio's effective mbedTLS build.
 * Seed material comes from the host OS CSPRNG, provisioned on the SD card; it
 * is replaced before opening TLS, so consecutive boots do not reuse it. */
#include "gc_tls.h"
#ifdef WANT_TLS
#include <gccore.h>
#include <network.h>
#include <ogc/lwp_watchdog.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "mbedtls/sha512.h"
#include "debuglog.h"

typedef struct {
    int fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
} tls_session;
static mutex_t tls_lock;
static int ready;
static char root[1024];
static mbedtls_x509_crt ca;
static mbedtls_ctr_drbg_context rng;
static unsigned char entropy_state[64];
mbedtls_ms_time_t mbedtls_ms_time(void) { return ticks_to_millisecs(gettime()); }

/* Keep this policy explicit at the connection too, as in GCRadio.  Only the
 * two wall-clock flags may be cleared; CA, signature and name errors survive. */
static int verify_without_rtc(void *ctx, mbedtls_x509_crt *crt,
    int depth, uint32_t *flags)
{
    (void)ctx; (void)crt;
    *flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
    if (*flags)
        DebugMark("tls: certificate depth %d rejected, flags 0x%08lx",
            depth, (unsigned long)*flags);
    return 0;
}

/* Gekko has no AES acceleration. Match GCRadio's ChaCha20-first preference. */
static const int ciphersuites[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    0
};

void gc_tls_init(const char *path)
{
    snprintf(root, sizeof(root), "%s", path);
    LWP_MutexInit(&tls_lock, false);
}

/* Hashing advances the provisioned seed, and the timebase adds diversity.
 * No claim of entropy is made for the timer: the SD seed is required. */
static int seed_bytes(void *unused, unsigned char *out, size_t len)
{
    (void)unused;
    while (len) {
        unsigned char input[72];
        u64 tick = gettime();
        size_t n = len < 64 ? len : 64;
        memcpy(input, entropy_state, 64);
        memcpy(input + 64, &tick, 8);
        if (mbedtls_sha512(input, sizeof(input), entropy_state, 0)) return -1;
        memcpy(out, entropy_state, n);
        out += n; len -= n;
        memset(input, 0, sizeof(input));
    }
    return 0;
}

static int random_bytes(void *ctx, unsigned char *out, size_t len)
{
    int r;
    LWP_MutexLock(tls_lock);
    r = mbedtls_ctr_drbg_random(ctx, out, len);
    LWP_MutexUnlock(tls_lock);
    return r;
}

static int prepare(char *error, size_t cap)
{
    char path[1100];
    FILE *f = NULL;
    unsigned char *pem = NULL, next_seed[64];
    long size;
    int ret = -1;
    LWP_MutexLock(tls_lock);
    if (ready) { ret = 0; goto done; }
    mbedtls_x509_crt_init(&ca);
    mbedtls_ctr_drbg_init(&rng);
    snprintf(path, sizeof(path), "%s/tls-seed.bin", root);
    f = fopen(path, "rb");
    if (!f || fread(entropy_state, 1, sizeof(entropy_state), f) != sizeof(entropy_state)) {
        snprintf(error, cap, "Missing TLS seed: run tools/prepare-sd.ps1"); goto done;
    }
    fclose(f); f = NULL;
    const unsigned char personalization[] = "WiiMC-GCN-Online";
    if (mbedtls_ctr_drbg_seed(&rng, seed_bytes, NULL, personalization, sizeof(personalization)-1) ||
        mbedtls_ctr_drbg_random(&rng, next_seed, sizeof(next_seed))) {
        snprintf(error, cap, "TLS random generator failed"); goto done;
    }
    /* Roll the seed through a temporary file and a rename, never in place.
     *
     * fopen(path, "wb") truncates before it writes. A console switched off
     * or reset in that window -- and this one gets reset a lot -- left a
     * zero-length tls-seed.bin, which is the only seed there is: HTTPS then
     * refuses to start until the card is prepared again from the PC.
     *
     * The rename is the commit point. FAT cannot rename onto an existing
     * name, so the old file is removed first; that leaves a window where
     * neither name is the seed, which is why the read of the old seed above
     * has already finished and next_seed is in memory. */
    {
        char temp[1100];
        int written;

        snprintf(temp, sizeof(temp), "%s/tls-seed.tmp", root);
        remove(temp);

        f = fopen(temp, "wb");
        written = f && fwrite(next_seed, 1, sizeof(next_seed), f) == sizeof(next_seed) && !fflush(f);
        if (f && fclose(f)) written = 0;
        f = NULL;

        if (!written) {
            remove(temp);
            snprintf(error, cap, "Cannot update TLS seed on SD card"); goto done;
        }

        if (remove(path) || rename(temp, path)) {
            /* The old seed is already gone or the rename failed: put the new
             * one where it belongs by copying, so the card is never left
             * without a seed. */
            f = fopen(path, "wb");
            written = f && fwrite(next_seed, 1, sizeof(next_seed), f) == sizeof(next_seed) && !fflush(f);
            if (f && fclose(f)) written = 0;
            f = NULL;
            remove(temp);

            if (!written) { snprintf(error, cap, "Cannot save TLS seed"); goto done; }
        }
    }
    snprintf(path, sizeof(path), "%s/ca.pem", root);
    f = fopen(path, "rb");
    if (!f) { snprintf(error, cap, "Missing apps/wiimc/ca.pem"); goto done; }
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 1 || size > 512*1024 || fseek(f, 0, SEEK_SET)) {
        snprintf(error, cap, "Invalid CA bundle size"); goto done;
    }
    pem = malloc(size+1);
    if (!pem || fread(pem, 1, size, f) != (size_t)size) {
        snprintf(error, cap, "Cannot read CA bundle"); goto done;
    }
    pem[size] = 0;
    if (mbedtls_x509_crt_parse(&ca, pem, size+1) < 0) {
        snprintf(error, cap, "Invalid CA bundle"); goto done;
    }
    ready = 1;
    ret = 0;
done:
    if (f) fclose(f);
    free(pem);
    memset(next_seed, 0, sizeof(next_seed));
    if (ret) { mbedtls_x509_crt_free(&ca); mbedtls_ctr_drbg_free(&rng); }
    LWP_MutexUnlock(tls_lock);
    return ret;
}

static int tls_send(void *ctx, const unsigned char *buf, size_t len)
{
    int r = net_send(((tls_session *)ctx)->fd, buf, len, 0);
    if (r == -EAGAIN || r == -EWOULDBLOCK || r == -EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r;
}
static int tls_recv(void *ctx, unsigned char *buf, size_t len)
{
    int r = net_recv(((tls_session *)ctx)->fd, buf, len, 0);
    if (r == -EAGAIN || r == -EWOULDBLOCK || r == -EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
    return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r;
}

void *gc_tls_open(int fd, const char *host, gc_net_cancel_fn cancel, void *opaque, char *error, size_t cap)
{
    int r;
    if (prepare(error, cap)) return NULL;
    tls_session *s = calloc(1, sizeof(*s));
    if (!s) { snprintf(error, cap, "Out of memory for TLS"); return NULL; }
    s->fd = fd;
    mbedtls_ssl_init(&s->ssl);
    mbedtls_ssl_config_init(&s->config);
    r = mbedtls_ssl_config_defaults(&s->config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (r) goto failed;
    mbedtls_ssl_conf_authmode(&s->config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_verify(&s->config, verify_without_rtc, NULL);
    mbedtls_ssl_conf_ca_chain(&s->config, &ca, NULL);
    mbedtls_ssl_conf_rng(&s->config, random_bytes, &rng);
    mbedtls_ssl_conf_ciphersuites(&s->config, ciphersuites);
    if ((r = mbedtls_ssl_setup(&s->ssl, &s->config)) || (r = mbedtls_ssl_set_hostname(&s->ssl, host))) goto failed;
    mbedtls_ssl_set_bio(&s->ssl, s, tls_send, tls_recv, NULL);
    u64 start = gettime();
    do {
        if (cancel && cancel(opaque)) { snprintf(error, cap, "Cancelled"); goto close; }
        r = mbedtls_ssl_handshake(&s->ssl);
        if (!r) {
            if (mbedtls_ssl_get_verify_result(&s->ssl) != 0) {
                r = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
                goto failed;
            }
            DebugMark("tls: CA and hostname verified, dates disabled (GameCube RTC), TLS 1.2");
            return s;
        }
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) goto failed;
        if (ticks_to_millisecs(gettime() - start) >= 25000) { snprintf(error, cap, "TLS handshake timed out"); goto close; }
        usleep(10000);
    } while (1);
failed:
    {
        uint32_t verify_flags = mbedtls_ssl_get_verify_result(&s->ssl);
        char detail[160];
        mbedtls_strerror(r, detail, sizeof(detail));
        DebugMark("tls: handshake -0x%04x (%s), verify 0x%08lx",
            (unsigned)-r, detail, (unsigned long)verify_flags);
        /* UINT32_MAX means verification has not taken place, for example
         * after a socket/protocol failure. It is not a CA/name rejection. */
        if (verify_flags && verify_flags != UINT32_MAX) {
            snprintf(error, cap, "TLS certificate rejected (0x%08lx): check host and CA bundle",
                (unsigned long)verify_flags);
        } else {
            snprintf(error, cap, "TLS -0x%04x: %s", (unsigned)-r, detail);
        }
    }
close:
    DebugMark("tls: %s", error);
    gc_tls_close(s);
    return NULL;
}
int gc_tls_io(void *session, void *buf, int len, int writing)
{
    tls_session *s = session;
    int r = writing ? mbedtls_ssl_write(&s->ssl, buf, len) : mbedtls_ssl_read(&s->ssl, buf, len);
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return -EAGAIN;
    if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
    return r < 0 ? -EIO : r;
}
void gc_tls_close(void *session)
{
    tls_session *s = session;
    if (!s) return;
    mbedtls_ssl_free(&s->ssl);
    mbedtls_ssl_config_free(&s->config);
    free(s);
}
#endif
