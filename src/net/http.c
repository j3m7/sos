// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "net/http.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/time.h"

#include "core/heaptag.h"
#include "core/log.h"
#include "shell/termtext.h"
#include "time/timeconv.h"

#define RX_SIZE 1024            // decrypted bytes waiting for the parser

struct http_req {
    http_state_t state;
    char method[16];
    http_url_t url;
    char url_text[HTTP_URL_MAX + 1];    // url as text (http_url())
    uint8_t max_redirects, redirects;
    bool allow_plain, allow_downgrade;
    uint32_t timeout_ms;
    tls_conn_t *tls;

    char *out;                  // request headers and body; freed once sent
    size_t out_len, out_sent;

    http_resp_t resp;
    uint8_t rx[RX_SIZE];
    size_t rx_pos, rx_len;

    uint64_t start_us, deadline_us, headers_us, end_us;
    char error[112];
};

static void fail(http_req_t *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(http_req_t *r, const char *fmt, ...) {
    if (r->state == HTTP_FAILED || r->state == HTTP_DONE)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->error, sizeof(r->error), fmt, ap);
    va_end(ap);
    r->state = HTTP_FAILED;
    r->end_us = time_us_64();
    log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING,
            "http: %s %s failed: %s", r->method, r->url.host, r->error);
}

static void finish(http_req_t *r) {
    r->state = HTTP_DONE;
    r->end_us = time_us_64();
    log_msg(SOS_LOG_DAEMON | SOS_LOG_INFO,
            "http: %s %s: %d, %llu bytes, %u ms", r->method, r->url.host,
            r->resp.status, (unsigned long long)r->resp.body_received, (unsigned)http_elapsed_ms(r));
}

static void refresh_deadline(http_req_t *r) {
    r->deadline_us = time_us_64() + (uint64_t)r->timeout_ms * 1000;
}

// Build the request for r->url and open its connection: at the start, and for each redirect.
// Returns NULL, or what's wrong (then nothing is left allocated in r).
static const char *begin(http_req_t *r, const char *content_type, const char *const *headers,
                         const void *body, size_t body_len) {
    const char *err;
    http_request_t req = {r->method, &r->url, content_type, headers, body_len};
    // Built straight into the heap buffer, not on the caller's stack, then trimmed to size.
    r->out = heap_alloc(HEAP_HTTP, HTTP_HEAD_MAX + body_len);
    if (!r->out)
        return "out of memory";
    size_t head_len = http_request_build(&req, r->out, HTTP_HEAD_MAX, &err);
    if (!head_len) {
        heap_release(HEAP_HTTP, r->out);
        r->out = NULL;
        return err;
    }
    if (body_len)
        memcpy(r->out + head_len, body, body_len);
    r->out_len = head_len + body_len;
    r->out_sent = 0;
    char *trimmed = heap_realloc(HEAP_HTTP, r->out, r->out_len);
    if (trimmed)
        r->out = trimmed;

    http_resp_init(&r->resp, !strcmp(r->method, "HEAD"));
    r->rx_pos = r->rx_len = 0;
    refresh_deadline(r);
    r->state = HTTP_CONNECTING;
    // No ALPN: HTTP/1.1 is the default without it, and tls/ would refuse a server that
    // doesn't do ALPN at all.
    r->tls = r->url.plain ? tls_open_plain(r->url.host, r->url.port, r->timeout_ms, TLS_USER)
                          : tls_open(r->url.host, r->url.port, NULL, r->timeout_ms, TLS_USER);
    if (!r->tls) {
        heap_release(HEAP_HTTP, r->out);
        r->out = NULL;
        return "out of memory";
    }
    return NULL;
}

http_req_t *http_start(const http_opts_t *o, const char **err) {
    *err = NULL;
    if (o->body_len > HTTP_BODY_MAX) {
        *err = "request body too large";
        return NULL;
    }
    if (o->body_len && !o->body) {
        *err = "no request body";
        return NULL;
    }
    http_req_t *r = heap_zalloc(HEAP_HTTP, sizeof(*r));
    if (!r) {
        *err = "out of memory";
        return NULL;
    }
    r->allow_plain = o->allow_plain;
    *err = http_url_parse_any(o->url, &r->url, r->allow_plain);
    if (*err) {
        heap_release(HEAP_HTTP, r);
        return NULL;
    }
    snprintf(r->method, sizeof(r->method), "%s", o->method ? o->method : "");
    if (o->max_redirects) {
        if ((strcmp(r->method, "GET") && strcmp(r->method, "HEAD")) || o->body_len ||
            o->content_type || (o->headers && *o->headers)) {
            heap_release(HEAP_HTTP, r);
            *err = "redirects are followed only for GET and HEAD without extra headers";
            return NULL;
        }
        r->max_redirects = o->max_redirects < HTTP_REDIRECTS_MAX ? o->max_redirects
                                                                 : HTTP_REDIRECTS_MAX;
        r->allow_downgrade = o->allow_plain && o->allow_downgrade;
    }
    http_url_format(&r->url, r->url_text, sizeof(r->url_text));
    r->timeout_ms = o->timeout_ms ? o->timeout_ms : HTTP_DEFAULT_TIMEOUT;
    r->start_us = time_us_64();
    *err = begin(r, o->content_type, o->headers, o->body, o->body_len);
    if (*err) {
        heap_release(HEAP_HTTP, r);
        return NULL;
    }
    return r;
}

// Make decrypted bytes available in rx. Returns 1 if there are some, 0 if none yet, or -1 if
// the connection has ended (the request is then HTTP_DONE or HTTP_FAILED).
static int fill(http_req_t *r) {
    if (r->rx_pos < r->rx_len)
        return 1;
    int n = tls_read(r->tls, r->rx, sizeof(r->rx));
    if (n > 0) {
        r->rx_pos = 0;
        r->rx_len = (size_t)n;
        return 1;
    }
    if (n == 0)
        return 0;
    // The connection ended: TLS_CLOSED is close_notify, or the FIN on plain http; TLS_FAILED is
    // a FIN without close_notify on https (or a reset: wolfSSL can't tell them apart), or a
    // reset on plain http. http_resp_end() says whether that completes the response.
    bool plain = r->url.plain;
    bool closed = tls_poll(r->tls) == TLS_CLOSED;
    http_end_t how = closed ? (plain ? HTTP_END_FIN : HTTP_END_CLOSE_NOTIFY)
                            : (plain ? HTTP_END_RESET : HTTP_END_FIN);
    bool to_close = r->resp.state == HTTP_P_BODY_TO_CLOSE;
    const char *why = http_resp_end(&r->resp, plain, how);
    if (!why)
        finish(r);
    else if (closed || (to_close && !plain))
        fail(r, "%s", why);
    else
        fail(r, "%s (%s)", why, tls_error(r->tls));
    return -1;
}

static void send_request(http_req_t *r) {
    for (int i = 0; i < 4 && r->out_sent < r->out_len; i++) {
        int n = tls_write(r->tls, r->out + r->out_sent, r->out_len - r->out_sent);
        if (n < 0) {
            fail(r, "sending the request: %s", tls_error(r->tls));
            return;
        }
        if (n == 0)
            return;
        r->out_sent += (size_t)n;
    }
    if (r->out_sent == r->out_len) {
        heap_release(HEAP_HTTP, r->out);
        r->out = NULL;
        r->state = HTTP_WAITING;
    }
}

// The headers of a redirect are in: follow it (a new request on a new connection) or fail.
// The body of the 3xx isn't read.
static void follow_redirect(http_req_t *r) {
    if (r->redirects >= r->max_redirects) {
        fail(r, "too many redirects (more than %u)", (unsigned)r->max_redirects);
        return;
    }
    // The Location is read into rx, whose bytes (the 3xx's body) are dropped anyway.
    const char *loc = http_resp_header(&r->resp, "location", (char *)r->rx, sizeof(r->rx));
    if (!loc) {
        fail(r, "redirect refused: Location too long");
        return;
    }
    const char *why = http_redirect_target(&r->url, loc, r->allow_plain, r->allow_downgrade,
                                           r->url_text, sizeof(r->url_text));
    if (why) {
        fail(r, "redirect refused: %s", why);
        return;
    }
    log_msg(SOS_LOG_DAEMON | SOS_LOG_INFO, "http: %d redirect to %s", r->resp.status, r->url.host);
    tls_close(r->tls);
    r->tls = NULL;
    r->redirects++;
    why = begin(r, NULL, NULL, NULL, 0);
    if (why)
        fail(r, "%s", why);
}

static void read_headers(http_req_t *r) {
    for (int i = 0; i < 8 && r->state == HTTP_WAITING; i++) {
        if (fill(r) <= 0)
            return;
        const uint8_t *body;
        size_t body_len;
        r->rx_pos += http_resp_feed(&r->resp, r->rx + r->rx_pos, r->rx_len - r->rx_pos, 0,
                                    &body, &body_len);
        if (r->resp.state == HTTP_P_ERROR) {
            fail(r, "bad response: %s", r->resp.error);
        } else if (http_resp_headers_done(&r->resp)) {
            if (r->max_redirects && http_status_redirects(r->resp.status) &&
                http_resp_has_header(&r->resp, "location")) {
                follow_redirect(r);
                return;
            }
            r->headers_us = time_us_64();
            refresh_deadline(r);
            if (r->resp.state == HTTP_P_DONE)
                finish(r);
            else
                r->state = HTTP_BODY;
        }
    }
}

static void check_timeout(http_req_t *r) {
    if (r->state < HTTP_DONE && time_us_64() > r->deadline_us) {
        static const char *const what[] = {"connecting", "sending the request",
                                           "waiting for the response", "reading the body"};
        fail(r, "timed out %s", what[r->state]);
    }
}

http_state_t http_poll(http_req_t *r) {
    switch (r->state) {
    case HTTP_CONNECTING: {
        tls_state_t s = tls_poll(r->tls);
        if (s == TLS_FAILED) {
            fail(r, "%s", tls_error(r->tls));
            break;
        }
        if (s == TLS_CLOSED) {
            fail(r, "connection closed during the handshake");
            break;
        }
        if (s != TLS_READY)
            break;
        r->state = HTTP_SENDING;
    }
        // fall through
    case HTTP_SENDING:
        send_request(r);
        if (r->state != HTTP_WAITING)
            break;
        // fall through
    case HTTP_WAITING:
        read_headers(r);
        break;
    default:
        break;
    }
    check_timeout(r);
    return r->state;
}

int http_read(http_req_t *r, void *buf, size_t len) {
    if (r->state == HTTP_DONE || r->state == HTTP_FAILED)
        return -1;
    if (r->state != HTTP_BODY) {
        http_poll(r);
        return r->state == HTTP_DONE || r->state == HTTP_FAILED ? -1 : 0;
    }
    if (len == 0)
        return 0;
    for (int i = 0; i < 16; i++) {
        int f = fill(r);
        if (f < 0)
            return -1;
        if (f == 0)
            break;
        const uint8_t *body;
        size_t body_len;
        r->rx_pos += http_resp_feed(&r->resp, r->rx + r->rx_pos, r->rx_len - r->rx_pos, len,
                                    &body, &body_len);
        if (r->resp.state == HTTP_P_ERROR) {
            fail(r, "bad response: %s", r->resp.error);
            return -1;
        }
        if (r->resp.state == HTTP_P_DONE)
            finish(r);
        if (body_len) {
            memcpy(buf, body, body_len);
            refresh_deadline(r);
            return (int)body_len;
        }
        if (r->state == HTTP_DONE)
            return -1;
    }
    check_timeout(r);
    return r->state == HTTP_FAILED ? -1 : 0;
}

const http_resp_t *http_response(const http_req_t *r) {
    return &r->resp;
}

const char *http_error(const http_req_t *r) {
    return r->error;
}

const tls_stats_t *http_tls_stats(const http_req_t *r) {
    return r->tls && !tls_is_plain(r->tls) ? tls_stats(r->tls) : NULL;
}

bool http_plain(const http_req_t *r) {
    return r->url.plain;
}

uint32_t http_headers_ms(const http_req_t *r) {
    return r->headers_us ? (uint32_t)((r->headers_us - r->start_us) / 1000) : 0;
}

uint32_t http_elapsed_ms(const http_req_t *r) {
    uint64_t end = r->end_us ? r->end_us : time_us_64();
    return (uint32_t)((end - r->start_us) / 1000);
}

unsigned http_redirects(const http_req_t *r) {
    return r->redirects;
}

const char *http_url(const http_req_t *r) {
    return r->url_text;
}

void http_close(http_req_t *r) {
    if (!r)
        return;
    tls_close(r->tls);
    heap_release(HEAP_HTTP, r->out);
    heap_release(HEAP_HTTP, r);
}

// --- the `http` command -----------------------------------------------------------------------------

#define STEP_US 5000

typedef struct {
    http_req_t *req;
    bool head;
    bool shown;                 // status line printed
    char last;                  // last body character printed
    termtext_t body_text;       // UTF-8 state across body pieces
    char buf[512];
    char text[512 + 3];
} http_job_t;

// Write server-supplied text through the terminal filter (shell/termtext.h): no escape
// sequences, raw or UTF-8-encoded C1 controls included. In pieces, and with shell_write
// rather than shell_printf, whose 256-byte buffer would cut long values short
// (Content-Security-Policy, say).
static void write_text(shell_t *sh, const char *s, size_t len) {
    termtext_t t = TERMTEXT_INIT;
    char out[128 + 3];
    while (len) {
        size_t n = len < 128 ? len : 128;
        shell_write(sh, out, termtext_filter(&t, (const uint8_t *)s, n, out));
        s += n;
        len -= n;
    }
    shell_write(sh, out, termtext_flush(&t, out));
}

static void print_headers(shell_t *sh, const http_resp_t *p) {
    const char *s = p->hdr, *end = p->hdr + p->hdr_len;
    while (s < end) {
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        const char *colon = memchr(s, ':', (size_t)(nl - s));
        shell_write(sh, s, (size_t)(colon - s));            // a token: plain ASCII
        shell_write(sh, ": ", 2);
        write_text(sh, colon + 1, (size_t)(nl - colon));    // the value and its '\n'
        s = nl + 1;
    }
}

// Body text for the terminal (shell/termtext.h); a UTF-8 character may span two pieces.
static size_t printable(http_job_t *j, size_t n) {
    size_t out = termtext_filter(&j->body_text, (const uint8_t *)j->buf, n, j->text);
    if (out)
        j->last = j->text[out - 1];
    return out;
}

static bool http_job_end(shell_t *sh, http_job_t *j) {
    http_req_t *r = j->req;
    size_t t = termtext_flush(&j->body_text, j->text);     // a UTF-8 character cut off
    if (t) {
        shell_write(sh, j->text, t);
        j->last = j->text[t - 1];
    }
    if (j->shown && j->last && j->last != '\n')
        shell_printf(sh, "\n");
    if (r->state == HTTP_FAILED) {
        shell_printf(sh, "http: %s\n", http_error(r));
    } else {
        const tls_stats_t *ts = http_tls_stats(r);
        shell_printf(sh, "--- %d, %llu bytes, %u ms (headers after %u ms; TLS handshake %u ms, "
                     "wolfSSL heap peak %u KB)\n",
                     r->resp.status, (unsigned long long)r->resp.body_received,
                     (unsigned)http_elapsed_ms(r), (unsigned)http_headers_ms(r),
                     ts ? (unsigned)(ts->handshake_us / 1000) : 0,
                     ts ? (unsigned)((ts->heap_peak + 1023) / 1024) : 0);
        if (ts && ts->cert_known) {
            char until[24] = "?";
            if (ts->cert_not_after)
                timeconv_format(ts->cert_not_after, until, sizeof(until));
            shell_printf(sh, "--- certificate: %s, from %s, valid until %s UTC\n",
                         ts->cert_subject, ts->cert_issuer, until);
        }
    }
    http_close(r);
    heap_release(HEAP_HTTP, j);
    return false;
}

static bool http_step(shell_t *sh, void *arg, bool cancel) {
    http_job_t *j = arg;
    if (cancel) {
        http_close(j->req);
        heap_release(HEAP_HTTP, j);
        return false;
    }
    uint64_t t0 = time_us_64();
    while (time_us_64() - t0 < STEP_US) {
        http_state_t s = http_poll(j->req);
        if (s == HTTP_FAILED)
            return http_job_end(sh, j);
        if (s < HTTP_BODY)
            return true;
        if (!j->shown) {
            const http_resp_t *p = http_response(j->req);
            shell_printf(sh, "HTTP/1.%d %d ", p->minor, p->status);
            write_text(sh, p->reason, strlen(p->reason));
            shell_write(sh, "\n", 1);
            if (j->head)
                print_headers(sh, p);
            j->shown = true;
        }
        int space = shell_write_space(sh) / 2;          // room for "\n" -> "\r\n"
        if (space < 64)
            return true;                                // wait for the client to read
        size_t want = (size_t)space < sizeof(j->buf) ? (size_t)space : sizeof(j->buf);
        int n = http_read(j->req, j->buf, want);
        if (n < 0)
            return http_job_end(sh, j);
        if (n == 0)
            return true;
        size_t t = printable(j, (size_t)n);
        if (t)
            shell_write(sh, j->text, t);
    }
    return true;
}

void http_command(shell_t *sh, char *args) {
    static const char usage[] = "usage: http get|head <url> | http post <url> <text>\n";
    char *verb = args ? strtok(args, " ") : NULL;
    char *url = verb ? strtok(NULL, " ") : NULL;
    char *text = url ? strtok(NULL, "") : NULL;
    if (!verb || !url) {
        shell_printf(sh, usage);
        return;
    }
    const char *method = !strcmp(verb, "get") ? "GET" : !strcmp(verb, "head") ? "HEAD"
                       : !strcmp(verb, "post") ? "POST" : NULL;
    if (!method || (!strcmp(method, "POST")) != (text != NULL)) {
        shell_printf(sh, usage);
        return;
    }
    // Any shell, SSH included: SSH users are the device's trusted owners, and Lua scripts they
    // start can make the same requests (sos.http).
    http_job_t *j = heap_zalloc(HEAP_HTTP, sizeof(*j));
    if (!j) {
        shell_printf(sh, "http: out of memory\n");
        return;
    }
    http_opts_t o = {
        .method = method,
        .url = url,
        .content_type = text ? "text/plain; charset=utf-8" : NULL,
        .body = text,
        .body_len = text ? strlen(text) : 0,
    };
    const char *err;
    j->req = http_start(&o, &err);
    if (!j->req) {
        shell_printf(sh, "http: %s\n", err);
        heap_release(HEAP_HTTP, j);
        return;
    }
    j->head = !strcmp(method, "HEAD");
    shell_run_job(sh, http_step, j);
}
