// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "tls/tls.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/memory.h>

#include "pico-sshd/ringbuffer.h"

#include "core/log.h"
#include "time/timesync.h"
#include "tls/certstore.h"
#include "tls/tls_roots.h"

#define TLS_MAX_CONNS 2         // open at once
#define TLS_HOST_MAX  63
#define TLS_ALPN_MAX  31

struct tls_conn {
    tls_state_t state;
    uint32_t id;                    // DNS callbacks find the connection by id (see conns[])
    char host[TLS_HOST_MAX + 1];
    char alpn[TLS_ALPN_MAX + 1];    // "" = none
    uint16_t port;
    uint64_t deadline_us;
    uint64_t connected_us;

    // Written in lwIP context (interrupt), read in the main loop under the lwIP lock.
    volatile bool dns_done, dns_ok;
    ip_addr_t addr;
    volatile bool connected, peer_closed, tcp_failed;
    volatile err_t tcp_err;
    struct tcp_pcb *pcb;
    ssh_ringbuffer_t rx;            // TCP payload not yet taken by wolfSSL; TCP_WND bytes

    WOLFSSL_CTX *ctx;
    WOLFSSL *ssl;
    int64_t heap_base;
    tls_stats_t stats;
    char error[96];
};

// Live connections, for the DNS callback (lwIP can't cancel a lookup, so its answer may arrive
// after tls_close()). Changed and searched only with the lwIP lock held.
static tls_conn_t *conns[TLS_MAX_CONNS];
static uint32_t next_id = 1;

// The connection whose handshake is running, for the certificate verify callback.
static tls_conn_t *verifying;

// --- heap accounting (all wolfSSL/wolfSSH allocations) ---------------------------------------

static int64_t heap_now, heap_peak;

static void *count_malloc(size_t n) {
    void *p = malloc(n);
    if (p) {
        heap_now += (int64_t)malloc_usable_size(p);
        if (heap_now > heap_peak)
            heap_peak = heap_now;
    }
    return p;
}

static void count_free(void *p) {
    if (p)
        heap_now -= (int64_t)malloc_usable_size(p);
    free(p);
}

static void *count_realloc(void *p, size_t n) {
    size_t old = p ? malloc_usable_size(p) : 0;
    void *q = realloc(p, n);
    if (q || n == 0) {
        heap_now += (int64_t)(q ? malloc_usable_size(q) : 0) - (int64_t)old;
        if (heap_now > heap_peak)
            heap_peak = heap_now;
    }
    return q;
}

void tls_init(void) {
    wolfSSL_SetAllocators(count_malloc, count_free, count_realloc);
}

// --- helpers -------------------------------------------------------------------------------------

static void fail(tls_conn_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(tls_conn_t *c, const char *fmt, ...) {
    if (c->state == TLS_FAILED)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->error, sizeof(c->error), fmt, ap);
    va_end(ap);
    c->state = TLS_FAILED;
}

static void update_heap_stats(tls_conn_t *c) {
    int64_t used = heap_peak - c->heap_base;
    if (used > 0 && (size_t)used > c->stats.heap_peak)
        c->stats.heap_peak = (size_t)used;
}

static void wolf_error(tls_conn_t *c, const char *what, int err) {
    char reason[WOLFSSL_MAX_ERROR_SZ];
    wolfSSL_ERR_error_string_n((unsigned long)err, reason, sizeof(reason));
    fail(c, "%s: %s (%d)", what, reason, err);
}

// --- lwIP side (interrupt context: no heap, no logging) ------------------------------------------

static tls_conn_t *find_conn(uint32_t id) {
    for (unsigned i = 0; i < TLS_MAX_CONNS; i++)
        if (conns[i] && conns[i]->id == id)
            return conns[i];
    return NULL;
}

static void dns_found(const char *name, const ip_addr_t *addr, void *arg) {
    (void)name;
    tls_conn_t *c = find_conn((uint32_t)(uintptr_t)arg);
    if (!c)
        return;                                 // closed while resolving
    if (addr)
        c->addr = *addr;
    c->dns_ok = addr != NULL;
    c->dns_done = true;
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)pcb;
    tls_conn_t *c = arg;
    if (err == ERR_OK)
        c->connected = true;
    return ERR_OK;
}

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)pcb;
    tls_conn_t *c = arg;
    if (!p || err != ERR_OK) {
        log_msg(SOS_LOG_DEBUG, "tls: tcp recv %s, err %d", p ? "data" : "FIN", (int)err);
        if (p)
            pbuf_free(p);
        c->peer_closed = true;
        return ERR_OK;
    }
    // The window only reopens as wolfSSL takes data (tcp_recved in io_recv), and rx holds
    // TCP_WND, so this fits; if not, ERR_MEM makes lwIP keep the data and offer it again.
    if (p->tot_len > ssh_ringbuffer_free(&c->rx))
        return ERR_MEM;
    for (struct pbuf *q = p; q; q = q->next)
        ssh_ringbuffer_add(&c->rx, q->payload, q->len, false);
    pbuf_free(p);
    return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err) {
    tls_conn_t *c = arg;
    log_msg(SOS_LOG_DEBUG, "tls: tcp error %d", (int)err);
    c->pcb = NULL;                              // lwIP has freed it
    c->tcp_err = err;
    c->tcp_failed = true;
}

// --- wolfSSL I/O callbacks (main loop) -------------------------------------------------------

static int io_recv(WOLFSSL *ssl, char *buf, int sz, void *ctx) {
    (void)ssl;
    tls_conn_t *c = ctx;
    cyw43_arch_lwip_begin();
    size_t n = ssh_ringbuffer_used(&c->rx);
    if (n == 0) {
        bool closed = c->peer_closed || c->tcp_failed;
        cyw43_arch_lwip_end();
        if (closed)
            log_msg(SOS_LOG_DEBUG, "tls: recv: closed (peer_closed %d, tcp_failed %d)",
                    c->peer_closed, c->tcp_failed);
        return closed ? WOLFSSL_CBIO_ERR_CONN_CLOSE : WOLFSSL_CBIO_ERR_WANT_READ;
    }
    if (n > (size_t)sz)
        n = (size_t)sz;
    ssh_ringbuffer_read(&c->rx, (uint8_t *)buf, n);
    if (c->pcb)
        tcp_recved(c->pcb, (u16_t)n);           // n <= rx size < 64K
    cyw43_arch_lwip_end();
    return (int)n;
}

static int io_send(WOLFSSL *ssl, char *buf, int sz, void *ctx) {
    (void)ssl;
    tls_conn_t *c = ctx;
    cyw43_arch_lwip_begin();
    if (!c->pcb) {
        cyw43_arch_lwip_end();
        return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    }
    // Queue what fits; wolfSSL calls again with the rest.
    u16_t len = tcp_sndbuf(c->pcb);
    if ((int)len > sz)
        len = (u16_t)sz;
    err_t err = len ? tcp_write(c->pcb, buf, len, TCP_WRITE_FLAG_COPY) : ERR_MEM;
    if (err == ERR_OK)
        tcp_output(c->pcb);
    cyw43_arch_lwip_end();
    return err == ERR_OK ? (int)len : WOLFSSL_CBIO_ERR_WANT_WRITE;
}

// Certificate dates can only be checked against a clock that is set. Before that (e.g. NTS
// setting the clock for the first time) accept a chain whose only fault is its dates; the
// signatures and the host name are still checked.
static int verify_cb(int preverify, WOLFSSL_X509_STORE_CTX *store) {
    if (preverify)
        return 1;
    if ((store->error == ASN_BEFORE_DATE_E || store->error == ASN_AFTER_DATE_E) &&
        !timesync_clock_set()) {
        if (verifying)
            verifying->stats.dates_unchecked = true;
        return 1;
    }
    return 0;
}

int tls_load_root(struct WOLFSSL_CTX *ctx, const uint8_t *der, size_t len) {
    word32 flags = timesync_clock_set() ? WOLFSSL_LOAD_VERIFY_DEFAULT_FLAGS
                                        : WOLFSSL_LOAD_FLAG_DATE_ERR_OKAY;
    return wolfSSL_CTX_load_verify_buffer_ex(ctx, der, (long)len, WOLFSSL_FILETYPE_ASN1, 0, flags);
}

// --- connection steps (main loop) ------------------------------------------------------------

static bool wolf_initialised;

static void start_connect(tls_conn_t *c) {
    if (ssh_ringbuffer_init(&c->rx, NULL, TCP_WND) != 0) {
        fail(c, "out of memory");
        return;
    }
    cyw43_arch_lwip_begin();
    struct tcp_pcb *pcb = tcp_new_ip_type(IP_GET_TYPE(&c->addr));
    if (pcb) {
        tcp_arg(pcb, c);
        tcp_recv(pcb, tcp_recv_cb);
        tcp_err(pcb, tcp_err_cb);
        if (tcp_connect(pcb, &c->addr, c->port, tcp_connected_cb) != ERR_OK) {
            tcp_abort(pcb);
            pcb = NULL;
        }
    }
    c->pcb = pcb;
    cyw43_arch_lwip_end();
    if (!pcb) {
        fail(c, "can't open a TCP connection");
        return;
    }
    c->state = TLS_CONNECTING;
}

static void start_handshake(tls_conn_t *c) {
    if (!wolf_initialised) {
        if (wolfSSL_Init() != WOLFSSL_SUCCESS) {
            fail(c, "wolfSSL init failed");
            return;
        }
        wolf_initialised = true;
    }
    c->ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    if (!c->ctx) {
        fail(c, "out of memory");
        return;
    }
    for (unsigned i = 0; i < tls_root_count; i++) {
        int r = tls_load_root(c->ctx, tls_roots[i].der, tls_roots[i].len);
        if (r != WOLFSSL_SUCCESS) {
            wolf_error(c, "loading root certificate", r);
            return;
        }
    }
    certstore_load(c->ctx);                    // approved extra roots from the SD card, if any
    wolfSSL_CTX_set_verify(c->ctx, WOLFSSL_VERIFY_PEER, verify_cb);
    wolfSSL_CTX_SetIORecv(c->ctx, io_recv);
    wolfSSL_CTX_SetIOSend(c->ctx, io_send);

    c->ssl = wolfSSL_new(c->ctx);
    if (!c->ssl) {
        fail(c, "out of memory");
        return;
    }
    wolfSSL_SetIOReadCtx(c->ssl, c);
    wolfSSL_SetIOWriteCtx(c->ssl, c);
    // Keep the handshake secrets until close: the keying-material exporter (NTS) needs them.
    wolfSSL_KeepArrays(c->ssl);
    // Key exchange: only groups with fast code here (X25519, P-256: ~0.1 s), and an X25519
    // share up front so there's no HelloRetryRequest. Offering everything let servers pick
    // P-521, which takes ~1.4 s per operation on generic math: time.cloudflare.com closed the
    // connection before such a handshake finished.
    int groups[] = {WOLFSSL_ECC_X25519, WOLFSSL_ECC_SECP256R1};
    if (wolfSSL_set_groups(c->ssl, groups, 2) != WOLFSSL_SUCCESS ||
        wolfSSL_UseKeyShare(c->ssl, WOLFSSL_ECC_X25519) != WOLFSSL_SUCCESS) {
        fail(c, "setting the key exchange groups failed");
        return;
    }
    if (wolfSSL_UseSNI(c->ssl, WOLFSSL_SNI_HOST_NAME, c->host, (word16)strlen(c->host)) !=
            WOLFSSL_SUCCESS ||
        wolfSSL_check_domain_name(c->ssl, c->host) != WOLFSSL_SUCCESS ||
        (c->alpn[0] && wolfSSL_UseALPN(c->ssl, c->alpn, (word32)strlen(c->alpn),
                                       WOLFSSL_ALPN_FAILED_ON_MISMATCH) != WOLFSSL_SUCCESS)) {
        fail(c, "setting up the TLS session failed");
        return;
    }
    c->state = TLS_HANDSHAKE;
}

static void handshake_step(tls_conn_t *c) {
    uint64_t t0 = time_us_64();
    verifying = c;
    int r = wolfSSL_connect(c->ssl);
    verifying = NULL;
    uint32_t dt = (uint32_t)(time_us_64() - t0);
    if (dt > c->stats.longest_step_us)
        c->stats.longest_step_us = dt;
    if (r == WOLFSSL_SUCCESS) {
        if (c->alpn[0]) {
            char *proto = NULL;
            word16 len = 0;
            if (wolfSSL_ALPN_GetProtocol(c->ssl, &proto, &len) != WOLFSSL_SUCCESS ||
                len != strlen(c->alpn) || memcmp(proto, c->alpn, len)) {
                fail(c, "server didn't accept ALPN '%s'", c->alpn);
                return;
            }
        }
        c->stats.handshake_us = time_us_64() - c->connected_us;
        const char *group = wolfSSL_get_curve_name(c->ssl);
        log_msg(SOS_LOG_DEBUG, "tls: handshake done, key exchange group %s",
                group ? group : "?");
        c->state = TLS_READY;
        return;
    }
    int err = wolfSSL_get_error(c->ssl, r);
    if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE)
        return;
    wolf_error(c, "TLS handshake", err);
}

// --- public --------------------------------------------------------------------------------------

tls_conn_t *tls_open(const char *host, uint16_t port, const char *alpn, uint32_t timeout_ms) {
    tls_conn_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    snprintf(c->host, sizeof(c->host), "%s", host);
    snprintf(c->alpn, sizeof(c->alpn), "%s", alpn ? alpn : "");
    c->port = port;
    c->deadline_us = time_us_64() + (uint64_t)timeout_ms * 1000;
    c->heap_base = heap_now;
    heap_peak = heap_now;
    c->state = TLS_RESOLVING;
    if (strlen(host) > TLS_HOST_MAX || (alpn && strlen(alpn) > TLS_ALPN_MAX)) {
        fail(c, "host name or ALPN too long");
        return c;
    }

    cyw43_arch_lwip_begin();
    unsigned slot;
    for (slot = 0; slot < TLS_MAX_CONNS && conns[slot]; slot++)
        ;
    if (slot < TLS_MAX_CONNS) {
        c->id = next_id++;
        conns[slot] = c;
        err_t err = dns_gethostbyname(c->host, &c->addr, dns_found, (void *)(uintptr_t)c->id);
        if (err == ERR_OK)
            c->dns_ok = c->dns_done = true;     // cached, or an IP address
        else if (err != ERR_INPROGRESS)
            c->dns_done = true;                 // dns_ok stays false
    }
    cyw43_arch_lwip_end();
    if (slot == TLS_MAX_CONNS)
        fail(c, "too many TLS connections");
    return c;
}

tls_state_t tls_poll(tls_conn_t *c) {
    switch (c->state) {
    case TLS_RESOLVING:
        if (c->dns_done) {
            if (!c->dns_ok)
                fail(c, "DNS lookup of %s failed", c->host);
            else
                start_connect(c);
        }
        break;
    case TLS_CONNECTING:
        if (c->tcp_failed) {
            fail(c, "TCP connect to %s:%u failed (%d)", c->host, c->port, (int)c->tcp_err);
        } else if (c->connected) {
            c->connected_us = time_us_64();
            start_handshake(c);
            if (c->state == TLS_HANDSHAKE)
                handshake_step(c);
        }
        break;
    case TLS_HANDSHAKE:
        handshake_step(c);
        break;
    default:
        break;
    }
    if (c->state < TLS_READY && time_us_64() > c->deadline_us) {
        static const char *const what[] = {"DNS lookup", "TCP connect", "TLS handshake"};
        fail(c, "%s timed out", what[c->state]);
    }
    update_heap_stats(c);
    return c->state;
}

int tls_read(tls_conn_t *c, void *buf, size_t len) {
    if (c->state != TLS_READY)
        return -1;
    int r = wolfSSL_read(c->ssl, buf, (int)len);
    update_heap_stats(c);
    if (r > 0)
        return r;
    int err = wolfSSL_get_error(c->ssl, r);
    if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE)
        return 0;
    if (err == WOLFSSL_ERROR_ZERO_RETURN)
        c->state = TLS_CLOSED;                  // close_notify
    else
        wolf_error(c, "TLS read", err);
    return -1;
}

int tls_write(tls_conn_t *c, const void *buf, size_t len) {
    if (c->state != TLS_READY)
        return -1;
    int r = wolfSSL_write(c->ssl, buf, (int)len);
    update_heap_stats(c);
    if (r > 0)
        return r;
    int err = wolfSSL_get_error(c->ssl, r);
    if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE)
        return 0;
    wolf_error(c, "TLS write", err);
    return -1;
}

void tls_close(tls_conn_t *c) {
    if (!c)
        return;
    if (c->ssl && c->state == TLS_READY)
        wolfSSL_shutdown(c->ssl);               // one attempt at close_notify; don't wait
    if (c->ssl)
        wolfSSL_free(c->ssl);
    if (c->ctx)
        wolfSSL_CTX_free(c->ctx);

    cyw43_arch_lwip_begin();
    for (unsigned i = 0; i < TLS_MAX_CONNS; i++)
        if (conns[i] == c)
            conns[i] = NULL;
    if (c->pcb) {
        tcp_arg(c->pcb, NULL);
        tcp_recv(c->pcb, NULL);
        tcp_err(c->pcb, NULL);
        if (tcp_close(c->pcb) != ERR_OK)
            tcp_abort(c->pcb);
        c->pcb = NULL;
    }
    cyw43_arch_lwip_end();

    if (c->rx.buf)
        ssh_ringbuffer_destroy(&c->rx);
    free(c);
}

const char *tls_error(const tls_conn_t *c) {
    return c->error;
}

const tls_stats_t *tls_stats(const tls_conn_t *c) {
    return &c->stats;
}

WOLFSSL *tls_session(tls_conn_t *c) {
    return c->ssl;
}
