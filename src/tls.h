// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// TLS 1.3 client connections over lwIP (raw TCP API), using wolfSSL with custom I/O callbacks.
// Non-blocking: tls_open() starts DNS, TCP connect and the handshake, and tls_poll(), called
// from the main loop, advances them. Main loop only (lwIP callbacks only buffer data).
//
// The server certificate is checked against the built-in roots (tls_roots.h) and the host name.
// Certificate dates are checked only when the clock is set (timesync); before that, a chain
// that is otherwise valid is accepted and the fact is logged, since the clock may be what this
// connection is about to set (NTS).

#ifndef SOS_TLS_H
#define SOS_TLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct WOLFSSL WOLFSSL;

typedef enum {
    TLS_RESOLVING,      // DNS lookup
    TLS_CONNECTING,     // TCP connect
    TLS_HANDSHAKE,      // TLS handshake and certificate check
    TLS_READY,          // tls_read()/tls_write() can be used
    TLS_CLOSED,         // the server closed the connection cleanly
    TLS_FAILED,         // see tls_error()
} tls_state_t;

typedef struct {
    uint64_t handshake_us;      // TCP connected -> handshake done
    uint32_t longest_step_us;   // longest single wolfSSL call (main loop blocked that long)
    size_t heap_peak;           // most wolfSSL heap in use at once since tls_open()
    bool dates_unchecked;       // certificate dates skipped (clock not set)
} tls_stats_t;

typedef struct tls_conn tls_conn_t;

// Install wolfSSL heap accounting (tls_stats_t.heap_peak). Call once in main(), before any
// wolfSSL/wolfSSH initialisation. The count covers every wolfSSL user (SSH sessions too).
void tls_init(void);

// Start a connection. alpn may be NULL. Returns NULL if out of memory; otherwise check
// tls_poll() for progress. timeout_ms covers DNS, connect and handshake together.
tls_conn_t *tls_open(const char *host, uint16_t port, const char *alpn, uint32_t timeout_ms);

// Advance the connection; returns its state.
tls_state_t tls_poll(tls_conn_t *c);

// Read up to len bytes. Returns the number read, 0 if nothing is available yet, or -1 when the
// connection is closed or failed (then see tls_poll()).
int tls_read(tls_conn_t *c, void *buf, size_t len);

// Write len bytes. Returns the number accepted (may be fewer), 0 if it would block, -1 on error.
int tls_write(tls_conn_t *c, const void *buf, size_t len);

// Close (sending close_notify when possible) and free everything. c may be NULL.
void tls_close(tls_conn_t *c);

// Why the connection failed, or "" if it hasn't.
const char *tls_error(const tls_conn_t *c);

const tls_stats_t *tls_stats(const tls_conn_t *c);

// The wolfSSL session, for protocol-specific calls (e.g. the keying-material exporter) once
// TLS_READY. NULL before the handshake starts.
WOLFSSL *tls_session(tls_conn_t *c);

#endif
