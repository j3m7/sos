// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "time/nts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "pico/cyw43_arch.h"
#include "pico/rand.h"
#include "pico/time.h"
#include "lwip/dns.h"
#include "lwip/udp.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/aes.h>

#include "core/heaptag.h"
#include "core/log.h"
#include "core/stackmon.h"
#include "time/nts_ke.h"
#include "time/nts_ntp.h"
#include "tls/tls.h"

#define KE_TIMEOUT_MS    20000  // whole key exchange: DNS, connect, handshake, records
#define QUERY_TIMEOUT_MS 5000   // NTP query: DNS and reply
#define MAX_DELAY_US     (2 * 1000000LL)    // round trips longer than this aren't trusted

// --- key exchange state --------------------------------------------------------------------------

typedef enum { KE_IDLE, KE_RUNNING, KE_DONE, KE_FAILED } ke_state_t;

static struct {
    ke_state_t state;
    char host[64];
    tls_conn_t *tls;
    uint64_t started_us, finished_us;
    uint8_t request[16];
    size_t request_len, request_sent;
    uint8_t *response;              // NTS_KE_RESPONSE_MAX bytes while running
    size_t response_len;
    tls_stats_t stats;              // copied from the TLS connection when it ends
    char error[160];
} ke;

// The outcome of the last successful exchange: keys, NTP server and the cookie jar (used one
// per query, replenished from each reply).
static nts_ke_result_t result;
static uint8_t key_c2s[NTS_KEY_LEN], key_s2c[NTS_KEY_LEN];
static bool have_keys;

// --- query state ---------------------------------------------------------------------------------

typedef enum { Q_IDLE, Q_RESOLVING, Q_WAITING, Q_DONE, Q_FAILED } q_state_t;

static struct {
    q_state_t state;
    char server[NTS_KE_SERVER_MAX + 1];
    uint16_t port;
    uint32_t id;                    // DNS callback matches on it (lookups can't be cancelled)
    struct udp_pcb *pcb;
    uint8_t xmt[8], uid[NTS_NTP_UID_LEN];
    int64_t t1_us;                  // system clock when the request was sent
    uint64_t started_us, finished_us;
    unsigned ignored;               // replies that failed a check (ignored, kept waiting)
    bool nak;                       // an NTS NAK came (unauthenticated): counts if nothing valid does
    const char *last_reason;
    // Results.
    int64_t offset_us, delay_us;
    uint8_t stratum;
    char error[96];

    // Written in lwIP context, read in the main loop.
    volatile bool dns_done, dns_ok;
    ip_addr_t addr;
    volatile bool got;
    volatile uint64_t rx_boot_us;   // boot timer when the reply arrived
    volatile size_t rx_len;
} q;

// The request and the reply, NTS_NTP_PACKET_MAX bytes each, in one heap block (HEAP_TIME) that
// exists only during a query: q_begin() allocates it and q_finish() frees it. q_rx is written
// by q_udp_recv() in lwIP context, which can only run while the query's pcb exists, and
// q_finish() removes the pcb before it frees the block. Once the request is sent, q_tx is the
// scratch space the reply is decrypted into.
static uint8_t *q_rx;                       // the reply, copied in lwIP context
static uint8_t *q_tx;

static void q_free_buffers(void) {
    heap_release(HEAP_TIME, q_rx);          // one block: q_tx is its second half
    q_rx = q_tx = NULL;
}

// --- AES-SIV-CMAC-256 for nts_ntp.c --------------------------------------------------------------

static bool siv_seal(void *ctx, const uint8_t *ad, size_t adlen, const uint8_t *nonce,
                     size_t noncelen, const uint8_t *pt, size_t ptlen, uint8_t *out) {
    static const uint8_t none[1];
    return wc_AesSivEncrypt(ctx, NTS_KEY_LEN, ad, (word32)adlen, nonce, (word32)noncelen,
                            pt ? pt : none, (word32)ptlen, out, out + NTS_NTP_TAG_LEN) == 0;
}

static bool siv_open(void *ctx, const uint8_t *ad, size_t adlen, const uint8_t *nonce,
                     size_t noncelen, const uint8_t *ct, size_t ctlen, uint8_t *pt) {
    uint8_t siv[NTS_NTP_TAG_LEN];
    memcpy(siv, ct, sizeof(siv));               // the API takes the tag as non-const
    return wc_AesSivDecrypt(ctx, NTS_KEY_LEN, ad, (word32)adlen, nonce, (word32)noncelen,
                            ct + NTS_NTP_TAG_LEN, (word32)(ctlen - NTS_NTP_TAG_LEN), siv,
                            pt) == 0;
}

static const nts_aead_t aead_c2s = {siv_seal, siv_open, key_c2s};
static const nts_aead_t aead_s2c = {siv_seal, siv_open, key_s2c};

static void forget_keys(void) {
    memset(&result, 0, sizeof(result));
    memset(key_c2s, 0, sizeof(key_c2s));
    memset(key_s2c, 0, sizeof(key_s2c));
    have_keys = false;
}

// --- key exchange --------------------------------------------------------------------------------

static void finish(bool ok) {
    if (ke.tls) {
        ke.stats = *tls_stats(ke.tls);
        tls_close(ke.tls);
        ke.tls = NULL;
    }
    heap_release(HEAP_TIME, ke.response);
    ke.response = NULL;
    ke.finished_us = time_us_64();
    ke.state = ok ? KE_DONE : KE_FAILED;
}

static void fail(const char *why) {
    snprintf(ke.error, sizeof(ke.error), "%s (sent %u/%u, received %u bytes)", why,
             (unsigned)ke.request_sent, (unsigned)ke.request_len, (unsigned)ke.response_len);
    finish(false);
    log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING,
            "nts: key exchange with %s failed: %s", ke.host, ke.error);
}

static bool export_key(WOLFSSL *ssl, bool s2c, uint8_t *key) {
    uint8_t ctx[5];
    nts_ke_exporter_context(result.aead, s2c, ctx);
    return wolfSSL_export_keying_material(ssl, key, NTS_KEY_LEN, NTS_KE_EXPORTER_LABEL,
                                          strlen(NTS_KE_EXPORTER_LABEL), ctx, sizeof(ctx),
                                          1) == WOLFSSL_SUCCESS;
}

static void succeed(void) {
    WOLFSSL *ssl = tls_session(ke.tls);
    if (!export_key(ssl, false, key_c2s) || !export_key(ssl, true, key_s2c)) {
        forget_keys();
        fail("exporting the keys failed");
        return;
    }
    have_keys = true;
    finish(true);
    unsigned total = 0;
    for (unsigned i = 0; i < result.cookie_count; i++)
        total += result.cookie_len[i];
    // Two lines: the log truncates at LOG_LINE_MAX (160).
    log_msg(SOS_LOG_DAEMON | SOS_LOG_NOTICE,
            "nts: key exchange with %s done: %u cookies (%u bytes), NTP at %s:%u",
            ke.host, result.cookie_count, total, result.server[0] ? result.server : ke.host,
            result.port ? result.port : NTS_NTP_PORT);
    log_msg(SOS_LOG_DAEMON | SOS_LOG_INFO,
            "nts: handshake %lu ms, longest step %lu ms, wolfSSL heap peak %u bytes",
            (unsigned long)(ke.stats.handshake_us / 1000),
            (unsigned long)(ke.stats.longest_step_us / 1000), (unsigned)ke.stats.heap_peak);
    if (ke.stats.dates_unchecked)
        log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING,
                "nts: certificate dates not checked (clock not set)");
}

static bool sync_active(void);

static bool ke_begin(const char *host) {
    if (ke.state == KE_RUNNING || q.state == Q_RESOLVING || q.state == Q_WAITING)
        return false;
    memset(&ke, 0, sizeof(ke));
    forget_keys();
    snprintf(ke.host, sizeof(ke.host), "%s", host ? host : NTS_DEFAULT_SERVER);
    ke.started_us = time_us_64();
    ke.state = KE_RUNNING;
    ke.request_len = nts_ke_build_request(ke.request, sizeof(ke.request));
    ke.response = heap_alloc(HEAP_TIME, NTS_KE_RESPONSE_MAX);
    ke.tls = tls_open(ke.host, NTS_KE_PORT, NTS_KE_ALPN, KE_TIMEOUT_MS, TLS_SYSTEM);
    if (!ke.response || !ke.tls) {
        fail("out of memory");
        return true;
    }
    log_msg(SOS_LOG_DAEMON | SOS_LOG_INFO, "nts: key exchange with %s:%u", ke.host, NTS_KE_PORT);
    return true;
}

bool nts_ke_start(const char *host) {
    return !sync_active() && ke_begin(host);
}

static void ke_poll(void) {
    if (ke.state != KE_RUNNING)
        return;
    tls_state_t st = tls_poll(ke.tls);
    if (st == TLS_FAILED) {
        fail(tls_error(ke.tls));
        return;
    }
    if (time_us_64() - ke.started_us > (uint64_t)KE_TIMEOUT_MS * 1000) {
        fail("timed out");
        return;
    }
    if (st != TLS_READY && st != TLS_CLOSED)
        return;

    while (st == TLS_READY && ke.request_sent < ke.request_len) {
        int n = tls_write(ke.tls, ke.request + ke.request_sent, ke.request_len - ke.request_sent);
        if (n < 0) {
            fail(tls_error(ke.tls));
            return;
        }
        if (n == 0)
            return;                             // try again next time round
        ke.request_sent += (size_t)n;
    }

    for (;;) {
        int n = 0;
        if (ke.response_len < NTS_KE_RESPONSE_MAX && tls_poll(ke.tls) == TLS_READY)
            n = tls_read(ke.tls, ke.response + ke.response_len,
                         NTS_KE_RESPONSE_MAX - ke.response_len);
        if (n > 0)
            ke.response_len += (size_t)n;
        const char *why = NULL;
        nts_ke_status_t ps = nts_ke_parse(ke.response, ke.response_len, &result, &why);
        if (ps == NTS_KE_OK) {
            succeed();
            return;
        }
        if (ps == NTS_KE_INVALID) {
            fail(why);
            return;
        }
        if (ke.response_len == NTS_KE_RESPONSE_MAX) {
            fail("response too long");
            return;
        }
        if (n > 0)
            continue;                           // more may be buffered already
        tls_state_t now = tls_poll(ke.tls);
        if (n < 0 || now == TLS_CLOSED || now == TLS_FAILED) {
            fail(now == TLS_FAILED ? tls_error(ke.tls)
                                   : "server closed the connection before End of Message");
            return;
        }
        return;                                 // wait for more data
    }
}

// --- NTS-protected NTP query ---------------------------------------------------------------------

static int64_t clock_now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static void random_bytes(uint8_t *out, size_t len) {
    while (len) {
        uint64_t r = get_rand_64();
        size_t n = len < 8 ? len : 8;
        memcpy(out, &r, n);
        out += n;
        len -= n;
    }
}

// lwIP context.
static void q_dns_found(const char *name, const ip_addr_t *addr, void *arg) {
    (void)name;
    if ((uint32_t)(uintptr_t)arg != q.id || q.state != Q_RESOLVING)
        return;
    if (addr)
        q.addr = *addr;
    q.dns_ok = addr != NULL;
    q.dns_done = true;
}

// lwIP context: keep the first datagram from the server; the main loop checks it.
static void q_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr,
                       u16_t port) {
    (void)arg;
    (void)pcb;
    if (!q.got && q.state == Q_WAITING && port == q.port && ip_addr_cmp(addr, &q.addr) &&
        p->tot_len <= NTS_NTP_PACKET_MAX) {
        q.rx_len = pbuf_copy_partial(p, q_rx, p->tot_len, 0);
        q.rx_boot_us = time_us_64();
        q.got = true;
    }
    pbuf_free(p);
}

static void q_close_pcb(void) {
    cyw43_arch_lwip_begin();
    if (q.pcb) {
        udp_remove(q.pcb);
        q.pcb = NULL;
    }
    cyw43_arch_lwip_end();
}

static void q_finish(bool ok, const char *why) {
    q_close_pcb();                          // no callback after this: the buffers can go
    q_free_buffers();
    q.finished_us = time_us_64();
    q.state = ok ? Q_DONE : Q_FAILED;
    if (!ok) {
        snprintf(q.error, sizeof(q.error), "%s", why);
        log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING, "nts: query to %s failed: %s", q.server, why);
    }
}

static const char *q_begin(void) {
    if (ke.state == KE_RUNNING || q.state == Q_RESOLVING || q.state == Q_WAITING)
        return "busy";
    if (!have_keys)
        return "no NTS keys: run 'nts ke' first";
    if (result.cookie_count == 0)
        return "no cookies left: run 'nts ke' again";
    q_free_buffers();                       // none: the last query freed them when it finished
    q_rx = heap_alloc(HEAP_TIME, 2 * NTS_NTP_PACKET_MAX);
    if (!q_rx)
        return "out of memory";
    q_tx = q_rx + NTS_NTP_PACKET_MAX;
    memset(&q, 0, sizeof(q));
    snprintf(q.server, sizeof(q.server), "%s", result.server[0] ? result.server : ke.host);
    q.port = result.port ? result.port : NTS_NTP_PORT;
    q.started_us = time_us_64();
    q.state = Q_RESOLVING;

    static uint32_t next_id = 1;
    cyw43_arch_lwip_begin();
    q.id = next_id++;
    err_t err = dns_gethostbyname(q.server, &q.addr, q_dns_found, (void *)(uintptr_t)q.id);
    if (err == ERR_OK)
        q.dns_ok = q.dns_done = true;
    else if (err != ERR_INPROGRESS)
        q.dns_done = true;
    cyw43_arch_lwip_end();
    return NULL;
}

const char *nts_query_start(void) {
    return sync_active() ? "busy (a sync is running)" : q_begin();
}

static void q_send(void) {
    // Use the newest cookie and ask for enough new ones to have NTS_NTP_MAX_COOKIES again.
    unsigned i = --result.cookie_count;
    unsigned placeholders = NTS_NTP_MAX_COOKIES - 1 - result.cookie_count;
    uint8_t nonce[NTS_NTP_NONCE_LEN];
    random_bytes(q.uid, sizeof(q.uid));
    random_bytes(nonce, sizeof(nonce));
    random_bytes(q.xmt, sizeof(q.xmt));         // the real send time stays here (RFC 9109)
    size_t len = nts_ntp_build_request(q_tx, NTS_NTP_PACKET_MAX, q.xmt, q.uid, result.cookies[i],
                                       result.cookie_len[i], placeholders, nonce, &aead_c2s);
    memset(result.cookies[i], 0, sizeof(result.cookies[i]));   // a cookie is used only once
    if (!len) {
        q_finish(false, "building the request failed");
        return;
    }

    cyw43_arch_lwip_begin();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    q.pcb = udp_new_ip_type(IP_GET_TYPE(&q.addr));
    err_t err = ERR_MEM;
    if (p && q.pcb) {
        memcpy(p->payload, q_tx, len);
        udp_recv(q.pcb, q_udp_recv, NULL);
        q.state = Q_WAITING;                    // before sending: the reply may come at once
        q.t1_us = clock_now_us();
        err = udp_sendto(q.pcb, p, &q.addr, q.port);
    }
    if (p)
        pbuf_free(p);
    cyw43_arch_lwip_end();
    if (err != ERR_OK)
        q_finish(false, "sending the request failed");
}

static void q_check_reply(void) {
    // T4: the clock now, minus the time since the reply arrived.
    int64_t t4 = clock_now_us() - (int64_t)(time_us_64() - q.rx_boot_us);
    nts_ntp_reply_t *r = heap_alloc(HEAP_TIME, sizeof(*r));
    if (!r) {
        q_finish(false, "out of memory");
        return;
    }
    const char *why = NULL;
    // The request has been sent: its buffer is free to decrypt the reply into.
    nts_ntp_status_t st = nts_ntp_parse_reply(q_rx, q.rx_len, q.xmt, q.uid, &aead_s2c, q_tx,
                                              NTS_NTP_PACKET_MAX, r, &why);
    if (st == NTS_NTP_INVALID) {
        // Not ours or not authentic: ignore it and keep waiting, so a forged packet can't
        // end the query.
        q.ignored++;
        q.last_reason = why;
        log_msg(SOS_LOG_DAEMON | SOS_LOG_INFO, "nts: ignored a reply from %s: %s", q.server, why);
        q.got = false;
        heap_release(HEAP_TIME, r);
        return;
    }
    if (st == NTS_NTP_NAK) {
        // A NAK isn't authenticated, so anyone who saw the request can send one. Keep waiting
        // for an authentic reply (RFC 8915 5.7); q_poll() acts on the NAK at the timeout.
        q.nak = true;
        q.got = false;
        heap_release(HEAP_TIME, r);
        return;
    }

    // Authentic: take the new cookies whatever the time says.
    for (unsigned i = 0; i < r->cookie_count && result.cookie_count < NTS_KE_MAX_COOKIES; i++) {
        unsigned n = result.cookie_count++;
        memcpy(result.cookies[n], r->cookies[i], r->cookie_len[i]);
        result.cookie_len[n] = r->cookie_len[i];
    }

    int64_t t2 = nts_ntp_to_unix_us(r->receive), t3 = nts_ntp_to_unix_us(r->transmit);
    const char *bad = NULL;
    if (r->leap == 3)
        bad = "server not synchronised (leap indicator 3)";
    else if (r->stratum == 0 || r->stratum > 15)
        bad = "bad stratum";
    else if (!r->receive || !r->transmit || t3 < t2)
        bad = "bad server timestamps";
    q.stratum = r->stratum;
    heap_release(HEAP_TIME, r);
    if (bad) {
        q_finish(false, bad);
        return;
    }
    nts_ntp_offset(q.t1_us, t2, t3, t4, &q.offset_us, &q.delay_us);
    if (q.delay_us < 0 || q.delay_us > MAX_DELAY_US) {
        q_finish(false, "round trip out of range");
        return;
    }
    q_finish(true, NULL);
    // Sign printed separately: for |offset| < 1 s the whole-seconds part is 0 and loses it.
    int64_t a = q.offset_us < 0 ? -q.offset_us : q.offset_us;
    log_msg(SOS_LOG_DAEMON | SOS_LOG_NOTICE,
            "nts: authenticated time from %s: offset %c%lld.%03lld s, delay %lld ms, stratum %u, "
            "%u cookies",
            q.server, q.offset_us < 0 ? '-' : '+', (long long)(a / 1000000),
            (long long)(a % 1000000 / 1000), (long long)(q.delay_us / 1000), q.stratum,
            result.cookie_count);
}

static void q_poll(void) {
    if (q.state != Q_RESOLVING && q.state != Q_WAITING)
        return;
    if (time_us_64() - q.started_us > (uint64_t)QUERY_TIMEOUT_MS * 1000) {
        char why[96];
        if (q.state == Q_RESOLVING)
            snprintf(why, sizeof(why), "DNS lookup timed out");
        else if (q.nak) {
            forget_keys();
            snprintf(why, sizeof(why), "server rejected the cookie (NTS NAK); run 'nts ke' again");
        }
        else if (q.ignored)
            snprintf(why, sizeof(why), "no valid reply (%u ignored, last: %s)", q.ignored,
                     q.last_reason);
        else
            snprintf(why, sizeof(why), "no reply within %d s", QUERY_TIMEOUT_MS / 1000);
        q_finish(false, why);
        return;
    }
    if (q.state == Q_RESOLVING) {
        if (!q.dns_done)
            return;
        if (!q.dns_ok) {
            q_finish(false, "DNS lookup failed");
            return;
        }
        q_send();
        return;
    }
    if (q.got)
        q_check_reply();
}

// --- sync: key exchange if needed, then several queries ------------------------------------------

static struct {
    bool active;
    nts_sync_state_t outcome;       // reported once by nts_sync_result()
    enum { SY_KE, SY_QUERY } phase;
    char host[64];
    bool ke_retried;                // after an NTS NAK
    unsigned samples, ok;
    int64_t best_offset_us, best_delay_us;
    char error[128];
} sy;

static bool sync_active(void) {
    return sy.active;
}

static void sync_finish(bool ok, const char *why) {
    sy.active = false;
    sy.outcome = ok ? NTS_SYNC_OK : NTS_SYNC_FAILED;
    if (!ok)
        snprintf(sy.error, sizeof(sy.error), "%s", why);
}

static void sync_query(void) {
    const char *err = q_begin();
    if (err)
        sync_finish(false, err);
    else
        sy.phase = SY_QUERY;
}

static void sync_ke(void) {
    if (!ke_begin(sy.host))
        sync_finish(false, "busy");
    else
        sy.phase = SY_KE;
}

bool nts_sync_start(const char *host) {
    if (sy.active || ke.state == KE_RUNNING || q.state == Q_RESOLVING || q.state == Q_WAITING)
        return false;
    memset(&sy, 0, sizeof(sy));
    sy.active = true;
    sy.outcome = NTS_SYNC_RUNNING;
    snprintf(sy.host, sizeof(sy.host), "%s", host ? host : NTS_DEFAULT_SERVER);
    if (!have_keys || result.cookie_count == 0 || strcmp(ke.host, sy.host))
        sync_ke();
    else
        sync_query();
    return true;
}

static void sync_poll(void) {
    if (!sy.active)
        return;
    if (sy.phase == SY_KE) {
        if (ke.state == KE_RUNNING)
            return;
        if (ke.state != KE_DONE) {
            sync_finish(false, ke.error);
            return;
        }
        sync_query();
        return;
    }
    if (q.state == Q_RESOLVING || q.state == Q_WAITING)
        return;
    sy.samples++;
    if (q.state == Q_DONE) {
        if (sy.ok == 0 || q.delay_us < sy.best_delay_us) {
            sy.best_delay_us = q.delay_us;
            sy.best_offset_us = q.offset_us;
        }
        sy.ok++;
    } else {
        snprintf(sy.error, sizeof(sy.error), "%s", q.error);
        if (!have_keys && !sy.ke_retried) {     // NTS NAK: new keys and cookies, once
            sy.ke_retried = true;
            sync_ke();
            return;
        }
    }
    if (sy.samples < NTS_SYNC_SAMPLES && have_keys && result.cookie_count > 0) {
        sync_query();
        return;
    }
    if (sy.ok)
        sync_finish(true, NULL);
    else
        sync_finish(false, sy.error[0] ? sy.error : "no reply");
}

nts_sync_state_t nts_sync_result(int64_t *offset_us, int64_t *delay_us, const char **error) {
    nts_sync_state_t st = sy.active ? NTS_SYNC_RUNNING : sy.outcome;
    if (st == NTS_SYNC_OK) {
        *offset_us = sy.best_offset_us;
        *delay_us = sy.best_delay_us;
    } else if (st == NTS_SYNC_FAILED) {
        *error = sy.error;
    }
    if (st == NTS_SYNC_OK || st == NTS_SYNC_FAILED)
        sy.outcome = NTS_SYNC_IDLE;
    return st;
}

void nts_poll(void) {
    ke_poll();
    q_poll();
    sync_poll();
}

// --- status --------------------------------------------------------------------------------------

static void print_ago(shell_t *sh, uint64_t since_us) {
    uint64_t s = (time_us_64() - since_us) / 1000000;
    if (s < 120)
        shell_printf(sh, "%llus ago", (unsigned long long)s);
    else
        shell_printf(sh, "%llum ago", (unsigned long long)(s / 60));
}

static void query_status(shell_t *sh) {
    switch (q.state) {
    case Q_IDLE:
        shell_printf(sh, "query:     not run ('nts query')\n");
        break;
    case Q_RESOLVING:
    case Q_WAITING:
        shell_printf(sh, "query:     running with %s\n", q.server);
        break;
    case Q_FAILED:
        shell_printf(sh, "query:     %s failed ", q.server);
        print_ago(sh, q.finished_us);
        shell_printf(sh, ": %s\n", q.error);
        break;
    case Q_DONE: {
        int64_t a = q.offset_us < 0 ? -q.offset_us : q.offset_us;
        shell_printf(sh, "query:     %s ok, ", q.server);
        print_ago(sh, q.finished_us);
        shell_printf(sh, " (authenticated)\n");
        shell_printf(sh, "offset:    %s%lld.%06lld s (server minus local clock)\n",
                     q.offset_us < 0 ? "-" : "+", (long long)(a / 1000000),
                     (long long)(a % 1000000));
        shell_printf(sh, "delay:     %lld.%03lld ms, stratum %u\n",
                     (long long)(q.delay_us / 1000), (long long)(q.delay_us % 1000), q.stratum);
        break;
    }
    }
}

void nts_status(shell_t *sh) {
    switch (ke.state) {
    case KE_IDLE:
        shell_printf(sh, "nts-ke:    not run ('nts ke [host]' to try one)\n");
        return;
    case KE_RUNNING:
        shell_printf(sh, "nts-ke:    running with %s\n", ke.host);
        return;
    case KE_FAILED:
        shell_printf(sh, "nts-ke:    %s failed ", ke.host);
        print_ago(sh, ke.finished_us);
        shell_printf(sh, ": %s\n", ke.error);
        break;
    case KE_DONE:
        shell_printf(sh, "nts-ke:    %s ok, ", ke.host);
        print_ago(sh, ke.finished_us);
        shell_printf(sh, "\n");
        break;
    }
    if (have_keys) {
        unsigned total = 0;
        for (unsigned i = 0; i < result.cookie_count; i++)
            total += result.cookie_len[i];
        shell_printf(sh, "cookies:   %u (%u bytes)\n", result.cookie_count, total);
        shell_printf(sh, "ntp:       %s:%u, AES-SIV-CMAC-256\n",
                     result.server[0] ? result.server : ke.host,
                     result.port ? result.port : NTS_NTP_PORT);
    } else if (ke.state == KE_DONE) {
        shell_printf(sh, "keys:      discarded (server sent NTS NAK)\n");
    }
    if (ke.stats.handshake_us)
        shell_printf(sh, "handshake: %lu ms, longest step %lu ms, wolfSSL heap peak %u bytes\n",
                     (unsigned long)(ke.stats.handshake_us / 1000),
                     (unsigned long)(ke.stats.longest_step_us / 1000),
                     (unsigned)ke.stats.heap_peak);
    if (ke.stats.dates_unchecked)
        shell_printf(sh, "note:      certificate dates not checked (clock was not set)\n");
    query_status(sh);
    shell_printf(sh, "stack:     %u peak of %u bytes (since boot)\n", (unsigned)stackmon_peak(),
                 (unsigned)stackmon_size());
}
