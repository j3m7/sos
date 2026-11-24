// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "nts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/time.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/ssl.h>

#include "log.h"
#include "nts_ke.h"
#include "stackmon.h"
#include "tls.h"

#define KE_TIMEOUT_MS 20000     // whole key exchange: DNS, connect, handshake, records

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

// The outcome of the last successful exchange (kept for the NTS NTP client).
static nts_ke_result_t result;
static uint8_t key_c2s[NTS_KEY_LEN], key_s2c[NTS_KEY_LEN];

static void finish(bool ok) {
    if (ke.tls) {
        ke.stats = *tls_stats(ke.tls);
        tls_close(ke.tls);
        ke.tls = NULL;
    }
    free(ke.response);
    ke.response = NULL;
    ke.finished_us = time_us_64();
    ke.state = ok ? KE_DONE : KE_FAILED;
}

static void fail(const char *why) {
    snprintf(ke.error, sizeof(ke.error), "%s (sent %u/%u, received %u bytes)", why,
             (unsigned)ke.request_sent, (unsigned)ke.request_len, (unsigned)ke.response_len);
    finish(false);
    log_msg(SOS_LOG_WARNING, "nts: key exchange with %s failed: %s", ke.host, ke.error);
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
        memset(&result, 0, sizeof(result));
        fail("exporting the keys failed");
        return;
    }
    finish(true);
    unsigned total = 0;
    for (unsigned i = 0; i < result.cookie_count; i++)
        total += result.cookie_len[i];
    // Two lines: the log truncates at LOG_LINE_MAX (160).
    log_msg(SOS_LOG_NOTICE, "nts: key exchange with %s done: %u cookies (%u bytes), NTP at %s:%u",
            ke.host, result.cookie_count, total, result.server[0] ? result.server : ke.host,
            result.port ? result.port : 123);
    log_msg(SOS_LOG_INFO, "nts: handshake %lu ms, longest step %lu ms, wolfSSL heap peak %u bytes",
            (unsigned long)(ke.stats.handshake_us / 1000),
            (unsigned long)(ke.stats.longest_step_us / 1000), (unsigned)ke.stats.heap_peak);
    if (ke.stats.dates_unchecked)
        log_msg(SOS_LOG_WARNING, "nts: certificate dates not checked (clock not set)");
}

bool nts_ke_start(const char *host) {
    if (ke.state == KE_RUNNING)
        return false;
    memset(&ke, 0, sizeof(ke));
    memset(&result, 0, sizeof(result));
    memset(key_c2s, 0, sizeof(key_c2s));
    memset(key_s2c, 0, sizeof(key_s2c));
    snprintf(ke.host, sizeof(ke.host), "%s", host ? host : NTS_DEFAULT_SERVER);
    ke.started_us = time_us_64();
    ke.state = KE_RUNNING;
    ke.request_len = nts_ke_build_request(ke.request, sizeof(ke.request));
    ke.response = malloc(NTS_KE_RESPONSE_MAX);
    ke.tls = tls_open(ke.host, NTS_KE_PORT, NTS_KE_ALPN, KE_TIMEOUT_MS);
    if (!ke.response || !ke.tls) {
        fail("out of memory");
        return true;
    }
    log_msg(SOS_LOG_INFO, "nts: key exchange with %s:%u", ke.host, NTS_KE_PORT);
    return true;
}

void nts_poll(void) {
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

static void print_ago(shell_t *sh, uint64_t since_us) {
    uint64_t s = (time_us_64() - since_us) / 1000000;
    if (s < 120)
        shell_printf(sh, "%llus ago", (unsigned long long)s);
    else
        shell_printf(sh, "%llum ago", (unsigned long long)(s / 60));
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
        unsigned total = 0;
        for (unsigned i = 0; i < result.cookie_count; i++)
            total += result.cookie_len[i];
        shell_printf(sh, "cookies:   %u (%u bytes)\n", result.cookie_count, total);
        shell_printf(sh, "ntp:       %s:%u\n", result.server[0] ? result.server : ke.host,
                     result.port ? result.port : 123);
        shell_printf(sh, "aead:      AES-SIV-CMAC-256, keys exported\n");
        break;
    }
    if (ke.stats.handshake_us)
        shell_printf(sh, "handshake: %lu ms, longest step %lu ms\n",
                     (unsigned long)(ke.stats.handshake_us / 1000),
                     (unsigned long)(ke.stats.longest_step_us / 1000));
    shell_printf(sh, "heap:      wolfSSL peak %u bytes during the exchange\n",
                 (unsigned)ke.stats.heap_peak);
    shell_printf(sh, "stack:     %u peak of %u bytes (since boot)\n", (unsigned)stackmon_peak(),
                 (unsigned)stackmon_size());
    if (ke.stats.dates_unchecked)
        shell_printf(sh, "note:      certificate dates not checked (clock was not set)\n");
}
