// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// HTTPS client: one request per connection (HTTP/1.1 with Connection: close) over tls/.
// Non-blocking, for the main loop: http_start() begins, http_poll() advances the connection and
// the request, and http_read() takes the response body as it arrives. The body is read only as
// fast as the caller takes it, so a slow consumer slows the server down (TCP flow control)
// instead of filling memory.
//
// https://, and plain http:// only for a caller that asks (http_opts_t.allow_plain: fetch,
// which prints the hash of what it saved). The server certificate is checked as for every TLS
// connection (tls/: built-in roots, the approved /certs/roots.pem, the host name). A plain
// request goes over a plain TCP connection from tls/ (tls_open_plain()): the same DNS, timeouts,
// flow control and connection slots, nothing protecting the bytes. Redirects are followed only
// when the caller asks (http_opts_t.max_redirects); otherwise the caller sees the 3xx status and
// the Location header. Compressed responses aren't requested.

#ifndef SOS_HTTP_H
#define SOS_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "net/http_msg.h"
#include "shell/shell.h"
#include "tls/tls.h"

#define HTTP_BODY_MAX        (16 * 1024)    // request body; it's copied
#define HTTP_HEAD_MAX        1536           // request line and headers, together
#define HTTP_DEFAULT_TIMEOUT 30000          // ms

typedef enum {
    HTTP_CONNECTING,    // DNS, TCP, TLS handshake (https)
    HTTP_SENDING,       // request going out
    HTTP_WAITING,       // waiting for the status line and headers
    HTTP_BODY,          // headers in (http_response()); read the body with http_read()
    HTTP_DONE,          // the whole response is in
    HTTP_FAILED,        // see http_error()
} http_state_t;

typedef struct {
    const char *method;             // "GET", "POST", "HEAD", ...
    const char *url;                // https://host[:port]/path?query (or http://, allow_plain)
    const char *content_type;       // for the body; NULL for none
    const char *const *headers;     // extra "Name: value" lines, NULL-terminated, or NULL
    const void *body;
    size_t body_len;                // up to HTTP_BODY_MAX
    uint32_t timeout_ms;            // until the headers arrive, then between pieces of the
                                    // body; 0: HTTP_DEFAULT_TIMEOUT
    // Redirects: 0 (the default) hands the caller a 3xx as it is. Otherwise a 301, 302, 303,
    // 307 or 308 with a Location is followed, up to this many hops (HTTP_REDIRECTS_MAX at
    // most): each a new connection, with its own certificate check and timeout. Only for GET
    // and HEAD without extra headers or a body (a header such as Authorization mustn't go to
    // whatever host a server names). A Location that can't be followed (http_redirect_target())
    // fails the request. http_redirects() and http_url() tell the caller where it went.
    uint8_t max_redirects;
    // Plain http: the URL, and redirects, may be http://. Nothing protects the bytes, nor the end
    // of a body without a length, which is taken at the TCP close (http_resp_t.ended_by_close).
    bool allow_plain;
    bool allow_downgrade;           // with allow_plain: a redirect from https to plain http may be
                                    // followed (otherwise it's refused; http to https always is)
} http_opts_t;

typedef struct http_req http_req_t;

// Start a request. The options are copied. Returns NULL with *err set if they're invalid
// (bad URL, header injection, body too large) or there's no memory.
http_req_t *http_start(const http_opts_t *o, const char **err);

// Advance the connection and request; returns the state. Call from the main loop until
// HTTP_BODY, then use http_read() (it also polls).
http_state_t http_poll(http_req_t *r);

// Read up to len bytes of the response body. Returns the number read, 0 if none are available
// yet, or -1 when there are no more: HTTP_DONE, or HTTP_FAILED (the body was cut short).
int http_read(http_req_t *r, void *buf, size_t len);

// The response: status, reason and headers (http_resp_header()). Valid from HTTP_BODY on.
const http_resp_t *http_response(const http_req_t *r);

const char *http_error(const http_req_t *r);

// TLS statistics (handshake time, wolfSSL heap peak), or NULL before the connection exists or
// when it's plain http.
const tls_stats_t *http_tls_stats(const http_req_t *r);

// True if the URL being requested now (the last hop's) is plain http.
bool http_plain(const http_req_t *r);

// Milliseconds from http_start() to the headers, and to the end (or now).
uint32_t http_headers_ms(const http_req_t *r);
uint32_t http_elapsed_ms(const http_req_t *r);

// Redirects followed so far, and the URL being requested now (the last hop's), as text.
unsigned http_redirects(const http_req_t *r);
const char *http_url(const http_req_t *r);

// Close the connection and free everything. r may be NULL.
void http_close(http_req_t *r);

// The `http` shell command: http get|head <url>, http post <url> <text>.
void http_command(shell_t *sh, char *args);

#endif
