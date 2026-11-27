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

#include "core/heap.h"
#include "core/heaptag.h"
#include "core/log.h"
#include "time/timesync.h"
#include "tls/certstore.h"
#include "tls/tls_roots.h"

#define TLS_HOST_MAX  63
#define TLS_ALPN_MAX  31

struct tls_conn {
    tls_state_t state;
    uint32_t id;                    // DNS callbacks find the connection by id (see conns[])
    char host[TLS_HOST_MAX + 1];
    char alpn[TLS_ALPN_MAX + 1];    // "" = none
    uint16_t port;
    tls_use_t use;
    uint64_t deadline_us;
    uint64_t connected_us;

    // Written in lwIP context (interrupt), read in the main loop under the lwIP lock.
    volatile bool dns_done, dns_ok;
    ip_addr_t addr;
    volatile bool connected, peer_closed, tcp_failed;
    volatile err_t tcp_err;
    uint64_t tcp_connected_us;      // when the server accepted (its timeouts count from here)
    uint64_t tcp_closed_us;         // when the server closed or the connection failed
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

// Kept apart from HEAP_NET's counter, which these hooks also feed: that one includes the
// application's own SSH, TLS and certificate blocks, and a connection's peak is wolfSSL's alone.
static int64_t heap_now;

// Raise each open connection's peak to what's in use now, counted from its tls_open(). Each
// connection keeps its own, so two at once (NTS and HTTPS) don't reset each other's.
static void note_peak(void) {
    for (unsigned i = 0; i < TLS_MAX_CONNS; i++) {
        tls_conn_t *c = conns[i];
        if (c && heap_now - c->heap_base > (int64_t)c->stats.heap_peak)
            c->stats.heap_peak = (size_t)(heap_now - c->heap_base);
    }
}

static void *count_malloc(size_t n) {
    void *p = heap_alloc(HEAP_NET, n);
    if (p) {
        heap_now += (int64_t)malloc_usable_size(p);
        note_peak();
    }
    return p;
}

static void count_free(void *p) {
    if (p)
        heap_now -= (int64_t)malloc_usable_size(p);
    heap_release(HEAP_NET, p);
}

static void *count_realloc(void *p, size_t n) {
    size_t old = p ? malloc_usable_size(p) : 0;
    void *q = heap_realloc(HEAP_NET, p, n);
    if (q || n == 0) {
        heap_now += (int64_t)(q ? malloc_usable_size(q) : 0) - (int64_t)old;
        note_peak();
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

static void wolf_error(tls_conn_t *c, const char *what, int err) {
    char reason[WOLFSSL_MAX_ERROR_SZ];
    wolfSSL_ERR_error_string_n((unsigned long)err, reason, sizeof(reason));
    fail(c, "%s: %s (%d)", what, reason, err);
    // Servers give up on slow clients (time.cloudflare.com: 2 s without progress), so say how
    // long this one waited. A time at such a limit means our side stalled: a lost packet not
    // yet resent (see LWIP_TCP_RTO_TIME in lwipopts.h), or a blocked main loop.
    cyw43_arch_lwip_begin();
    uint64_t connected = c->tcp_connected_us, closed = c->tcp_closed_us;
    bool reset = c->tcp_failed;
    cyw43_arch_lwip_end();
    if (closed && connected)
        log_msg(SOS_LOG_WARNING, "tls: %s %s the connection %lu ms after accepting it", c->host,
                reset ? "reset" : "closed", (unsigned long)((closed - connected) / 1000));
}

// --- lwIP side (interrupt context: no heap, no logging; the main loop reads what they set) -------

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
    if (err == ERR_OK) {
        c->tcp_connected_us = time_us_64();
        c->connected = true;
    }
    return ERR_OK;
}

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)pcb;
    tls_conn_t *c = arg;
    if (!p || err != ERR_OK) {              // the main loop logs it (io_recv)
        if (p)
            pbuf_free(p);
        if (!c->tcp_closed_us)
            c->tcp_closed_us = time_us_64();
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
    c->pcb = NULL;                              // lwIP has freed it
    c->tcp_err = err;
    if (!c->tcp_closed_us)
        c->tcp_closed_us = time_us_64();
    c->tcp_failed = true;
}

// --- wolfSSL I/O callbacks (main loop) -------------------------------------------------------

static int io_recv(WOLFSSL *ssl, char *buf, int sz, void *ctx) {
    (void)ssl;
    tls_conn_t *c = ctx;
    cyw43_arch_lwip_begin();
    size_t n = ssh_ringbuffer_used(&c->rx);
    if (n == 0) {
        bool fin = c->peer_closed, failed = c->tcp_failed;
        err_t tcp_err = c->tcp_err;
        cyw43_arch_lwip_end();
        bool closed = fin || failed;
        // What the callbacks saw, said here: they run in interrupt context and don't log.
        if (closed)
            log_msg(SOS_LOG_DEBUG,
                    "tls: recv: closed (peer_closed %d, tcp_failed %d, tcp error %d)", fin, failed,
                    (int)tcp_err);
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

static bool setup_session(tls_conn_t *c);

static void start_connect(tls_conn_t *c) {
    // Everything slow comes first (trusted roots, the SD card's root file): a server starts
    // timing us when it accepts the connection, so the ClientHello must follow at once.
    uint64_t t0 = time_us_64();
    if (!setup_session(c))
        return;
    log_msg(SOS_LOG_DEBUG, "tls: session setup %lu ms", (unsigned long)((time_us_64() - t0) / 1000));
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
        // TLS records are written whole; with Nagle, a record waits for the previous one to be
        // acknowledged (e.g. the first request behind the handshake's Finished).
        tcp_nagle_disable(pcb);
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

// Create the wolfSSL session and load the trust anchors. False if it failed (see c->error).
static bool setup_session(tls_conn_t *c) {
    if (!wolf_initialised) {
        if (wolfSSL_Init() != WOLFSSL_SUCCESS) {
            fail(c, "wolfSSL init failed");
            return false;
        }
        wolf_initialised = true;
    }
    c->ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    if (!c->ctx) {
        fail(c, "out of memory");
        return false;
    }
    for (unsigned i = 0; i < tls_root_count; i++) {
        int r = tls_load_root(c->ctx, tls_roots[i].der, tls_roots[i].len);
        if (r != WOLFSSL_SUCCESS) {
            wolf_error(c, "loading root certificate", r);
            return false;
        }
    }
    certstore_load(c->ctx);                    // approved extra roots from the SD card, if any
    wolfSSL_CTX_set_verify(c->ctx, WOLFSSL_VERIFY_PEER, verify_cb);
    wolfSSL_CTX_SetIORecv(c->ctx, io_recv);
    wolfSSL_CTX_SetIOSend(c->ctx, io_send);

    c->ssl = wolfSSL_new(c->ctx);
    if (!c->ssl) {
        fail(c, "out of memory");
        return false;
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
        return false;
    }
    if (wolfSSL_UseSNI(c->ssl, WOLFSSL_SNI_HOST_NAME, c->host, (word16)strlen(c->host)) !=
            WOLFSSL_SUCCESS ||
        wolfSSL_check_domain_name(c->ssl, c->host) != WOLFSSL_SUCCESS ||
        (c->alpn[0] && wolfSSL_UseALPN(c->ssl, c->alpn, (word32)strlen(c->alpn),
                                       WOLFSSL_ALPN_FAILED_ON_MISMATCH) != WOLFSSL_SUCCESS)) {
        fail(c, "setting up the TLS session failed");
        return false;
    }
    return true;
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

tls_conn_t *tls_open(const char *host, uint16_t port, const char *alpn, uint32_t timeout_ms,
                     tls_use_t use) {
    size_t free_heap = heap_free();         // before this connection's own allocation
    tls_conn_t *c = heap_zalloc(HEAP_NET, sizeof(*c));
    if (!c)
        return NULL;
    snprintf(c->host, sizeof(c->host), "%s", host);
    snprintf(c->alpn, sizeof(c->alpn), "%s", alpn ? alpn : "");
    c->port = port;
    c->use = use;
    c->deadline_us = time_us_64() + (uint64_t)timeout_ms * 1000;
    c->heap_base = heap_now;
    c->state = TLS_RESOLVING;
    if (strlen(host) > TLS_HOST_MAX || (alpn && strlen(alpn) > TLS_ALPN_MAX)) {
        fail(c, "host name or ALPN too long");
        return c;
    }

    cyw43_arch_lwip_begin();
    unsigned slot = TLS_MAX_CONNS, open = 0, open_user = 0, opening = 0;
    for (unsigned i = 0; i < TLS_MAX_CONNS; i++) {
        if (!conns[i]) {
            if (slot == TLS_MAX_CONNS)
                slot = i;
        } else {
            open++;
            open_user += conns[i]->use == TLS_USER;
            tls_state_t st = conns[i]->state;              // its handshake's peak still to come
            opening += st == TLS_RESOLVING || st == TLS_CONNECTING || st == TLS_HANDSHAKE;
        }
    }
    const char *refused = tls_admit(open, open_user, opening, use, free_heap);
    if (refused)
        slot = TLS_MAX_CONNS;
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
        fail(c, refused ? refused : "too many TLS connections open");
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
            cyw43_arch_lwip_begin();
            c->connected_us = c->tcp_connected_us;
            cyw43_arch_lwip_end();
            c->state = TLS_HANDSHAKE;
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
    return c->state;
}

int tls_read(tls_conn_t *c, void *buf, size_t len) {
    if (c->state != TLS_READY)
        return -1;
    int r = wolfSSL_read(c->ssl, buf, (int)len);
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
    heap_release(HEAP_NET, c);
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
