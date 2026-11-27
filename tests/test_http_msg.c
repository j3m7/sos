// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/net/http_msg.c: URL parsing, request building, response parsing.

#include <stdint.h>
#include <stdlib.h>

#include "net/http_msg.h"
#include "test.h"

// --- URLs ----------------------------------------------------------------------------------------

TEST(url_basic) {
    http_url_t u;
    CHECK(http_url_parse("https://ntfy.sh/mytopic", &u) == NULL);
    CHECK(!strcmp(u.host, "ntfy.sh"));
    CHECK_EQ(u.port, 443);
    CHECK(!strcmp(u.path, "/mytopic"));

    CHECK(http_url_parse("HTTPS://Example.COM", &u) == NULL);
    CHECK(!strcmp(u.host, "example.com"));
    CHECK(!strcmp(u.path, "/"));

    CHECK(http_url_parse("https://h.example:8443/a/b?x=1&y=%20#frag", &u) == NULL);
    CHECK(!strcmp(u.host, "h.example"));
    CHECK_EQ(u.port, 8443);
    CHECK(!strcmp(u.path, "/a/b?x=1&y=%20"));

    CHECK(http_url_parse("https://h?q=1", &u) == NULL);
    CHECK(!strcmp(u.path, "/?q=1"));
    CHECK(http_url_parse("https://192.168.1.10:1/", &u) == NULL);
    CHECK(!strcmp(u.host, "192.168.1.10"));
    CHECK_EQ(u.port, 1);
}

TEST(url_refused) {
    http_url_t u;
    CHECK(http_url_parse(NULL, &u) != NULL);
    CHECK(http_url_parse("", &u) != NULL);
    CHECK(http_url_parse("http://example.com/", &u) != NULL);
    CHECK(http_url_parse("ftp://example.com/", &u) != NULL);
    CHECK(http_url_parse("https:/", &u) != NULL);
    CHECK(http_url_parse("https://", &u) != NULL);
    CHECK(http_url_parse("https:///path", &u) != NULL);
    CHECK(http_url_parse("https://user:pw@example.com/", &u) != NULL);
    CHECK(http_url_parse("https://[::1]/", &u) != NULL);
    CHECK(http_url_parse("https://exa_mple.com/", &u) != NULL);
    CHECK(http_url_parse("https://example.com:/", &u) != NULL);
    CHECK(http_url_parse("https://example.com:0/", &u) != NULL);
    CHECK(http_url_parse("https://example.com:65536/", &u) != NULL);
    CHECK(http_url_parse("https://example.com:123456/", &u) != NULL);
    CHECK(http_url_parse("https://example.com:44x/", &u) != NULL);
    CHECK(http_url_parse("https://example.com/a b", &u) != NULL);
    CHECK(http_url_parse("https://example.com/a\r\nX: y", &u) != NULL);
    CHECK(http_url_parse("https://example.com/\x7f", &u) != NULL);
    CHECK(http_url_parse("https://example.com/caf\xc3\xa9", &u) != NULL);

    char long_host[HTTP_HOST_MAX + 20] = "https://";
    memset(long_host + 8, 'a', HTTP_HOST_MAX + 1);
    long_host[8 + HTTP_HOST_MAX + 1] = '\0';
    CHECK(http_url_parse(long_host, &u) != NULL);
    long_host[8 + HTTP_HOST_MAX] = '\0';             // exactly the limit
    CHECK(http_url_parse(long_host, &u) == NULL);

    static char long_path[HTTP_PATH_MAX + 64];
    strcpy(long_path, "https://h/");
    memset(long_path + 10, 'p', HTTP_PATH_MAX);      // '/' + HTTP_PATH_MAX characters
    CHECK(http_url_parse(long_path, &u) != NULL);
    long_path[10 + HTTP_PATH_MAX - 1] = '\0';        // exactly the limit
    CHECK(http_url_parse(long_path, &u) == NULL);
    CHECK_EQ(strlen(u.path), HTTP_PATH_MAX);
}

// --- requests ------------------------------------------------------------------------------------

static char req_buf[2048];

static size_t build(const char *method, const char *url, const char *ctype,
                    const char *const *headers, size_t body_len, const char **err) {
    http_url_t u;
    if (http_url_parse(url, &u)) {
        *err = "url";
        return 0;
    }
    http_request_t r = {method, &u, ctype, headers, body_len};
    return http_request_build(&r, req_buf, sizeof(req_buf), err);
}

TEST(request_get) {
    const char *err;
    size_t n = build("GET", "https://example.com/x?y=1", NULL, NULL, 0, &err);
    CHECK(err == NULL);
    const char *want = "GET /x?y=1 HTTP/1.1\r\n"
                       "Host: example.com\r\n"
                       "User-Agent: sos\r\n"
                       "Accept-Encoding: identity\r\n"
                       "Connection: close\r\n"
                       "\r\n";
    CHECK_EQ(n, strlen(want));
    CHECK(!strcmp(req_buf, want));
}

TEST(request_post) {
    const char *err;
    const char *hdrs[] = {"Title: Hello", "Authorization: Bearer abc.def", NULL};
    size_t n = build("POST", "https://ntfy.example:8443/t", "text/plain; charset=utf-8", hdrs, 5,
                     &err);
    CHECK(err == NULL);
    const char *want = "POST /t HTTP/1.1\r\n"
                       "Host: ntfy.example:8443\r\n"
                       "User-Agent: sos\r\n"
                       "Accept-Encoding: identity\r\n"
                       "Connection: close\r\n"
                       "Content-Type: text/plain; charset=utf-8\r\n"
                       "Content-Length: 5\r\n"
                       "Title: Hello\r\n"
                       "Authorization: Bearer abc.def\r\n"
                       "\r\n";
    CHECK_EQ(n, strlen(want));
    CHECK(!strcmp(req_buf, want));

    // POST with no body still says Content-Length: 0.
    n = build("POST", "https://h/", NULL, NULL, 0, &err);
    CHECK(n && strstr(req_buf, "Content-Length: 0\r\n"));
    // Own User-Agent replaces the default.
    const char *ua[] = {"user-agent: thing/1", NULL};
    n = build("GET", "https://h/", NULL, ua, 0, &err);
    CHECK(n && !strstr(req_buf, "User-Agent: sos") && strstr(req_buf, "user-agent: thing/1\r\n"));
}

TEST(request_refused) {
    const char *err;
    CHECK(build("get", "https://h/", NULL, NULL, 0, &err) == 0 && err);
    CHECK(build("", "https://h/", NULL, NULL, 0, &err) == 0 && err);
    CHECK(build("GET /evil", "https://h/", NULL, NULL, 0, &err) == 0 && err);
    CHECK(build("ABCDEFGHIJKLMNOP", "https://h/", NULL, NULL, 0, &err) == 0 && err);
    CHECK(build("POST", "https://h/", "text/plain\r\nX: y", NULL, 0, &err) == 0 && err);

    const char *const bad[][2] = {
        {"X-Ok: fine\r\nInjected: yes", NULL}, {"X-Nl: a\nb", NULL}, {"NoColon", NULL},
        {": empty name", NULL}, {"Bad Name: x", NULL}, {"Host: other.example", NULL},
        {"content-length: 9", NULL}, {"Transfer-Encoding: chunked", NULL},
        {"Connection: keep-alive", NULL}, {"Accept-Encoding: gzip", NULL},
        {"Content-Type: x/y", NULL}, {"X-Del: \x7f", NULL},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        err = NULL;
        CHECK(build("GET", "https://h/", NULL, bad[i], 0, &err) == 0);
        CHECK(err != NULL);
    }
    // Tabs are allowed in values.
    const char *tab[] = {"X-Tab: a\tb", NULL};
    CHECK(build("GET", "https://h/", NULL, tab, 0, &err) != 0);

    // Too long for the buffer.
    http_url_t u;
    http_url_parse("https://h/", &u);
    http_request_t r = {"GET", &u, NULL, NULL, 0};
    char small[40];
    CHECK(http_request_build(&r, small, sizeof(small), &err) == 0 && err);
}

// --- responses -----------------------------------------------------------------------------------

typedef struct {
    http_resp_t p;
    char body[8192];
    size_t body_len;
} result_t;

// Feed the whole response in pieces of `step` bytes (0: all at once), taking at most max_body
// bytes of body per call. Then, if eof, tell the parser the connection ended.
static void run(result_t *r, const char *resp, size_t len, bool head, size_t step,
                size_t max_body, bool eof) {
    http_resp_init(&r->p, head);
    r->body_len = 0;
    size_t at = 0;
    while (at < len && r->p.state != HTTP_P_DONE && r->p.state != HTTP_P_ERROR) {
        size_t piece = step ? step : len;
        if (piece > len - at)
            piece = len - at;
        size_t off = 0;
        while (off < piece) {
            const uint8_t *body;
            size_t body_len;
            size_t used = http_resp_feed(&r->p, (const uint8_t *)resp + at + off, piece - off,
                                         max_body, &body, &body_len);
            if (body_len) {
                CHECK(body_len <= max_body);
                CHECK(body >= (const uint8_t *)resp + at + off);
                CHECK(body + body_len <= (const uint8_t *)resp + at + off + used);
                memcpy(r->body + r->body_len, body, body_len);
                r->body_len += body_len;
            }
            if (used == 0)
                break;                          // done, error, or (max_body 0) waiting
            off += used;
        }
        at += piece;
        if (off < piece)
            break;
    }
    if (eof)
        http_resp_eof(&r->p);
}

// Run a response all at once, byte by byte, and in 7-byte pieces with small body reads; all
// must give the same outcome. Returns the byte-by-byte result.
static result_t results[3];
static result_t *run_all(const char *resp, bool head, bool eof) {
    size_t len = strlen(resp);
    run(&results[0], resp, len, head, 0, 100000, eof);
    run(&results[1], resp, len, head, 1, 100000, eof);
    run(&results[2], resp, len, head, 7, 3, eof);
    for (int i = 1; i < 3; i++) {
        CHECK_EQ(results[i].p.state, results[0].p.state);
        CHECK_EQ(results[i].p.status, results[0].p.status);
        CHECK_EQ(results[i].body_len, results[0].body_len);
        CHECK_MEM(results[i].body, results[0].body, results[0].body_len);
        CHECK(results[i].p.error == results[0].p.error);
    }
    return &results[1];
}

static bool header_is(const http_resp_t *p, const char *name, const char *value) {
    char buf[256];
    const char *v = http_resp_header(p, name, buf, sizeof(buf));
    return value ? v && !strcmp(v, value) : v == NULL;
}

TEST(resp_content_length) {
    result_t *r = run_all("HTTP/1.1 200 OK\r\n"
                          "Content-Type:  text/plain \r\n"
                          "Content-Length: 11\r\n"
                          "X-Empty:\r\n"
                          "\r\n"
                          "hello world", false, false);
    CHECK_EQ(r->p.state, HTTP_P_DONE);
    CHECK_EQ(r->p.status, 200);
    CHECK_EQ(r->p.minor, 1);
    CHECK(!strcmp(r->p.reason, "OK"));
    CHECK_EQ(r->body_len, 11);
    CHECK_MEM(r->body, "hello world", 11);
    CHECK(header_is(&r->p, "content-type", "text/plain"));
    CHECK(header_is(&r->p, "Content-Type", "text/plain"));
    CHECK(header_is(&r->p, "CONTENT-LENGTH", "11"));
    CHECK(header_is(&r->p, "x-empty", ""));
    CHECK(header_is(&r->p, "content", NULL));
    CHECK(header_is(&r->p, "missing", NULL));
    CHECK_EQ(r->p.body_received, 11);
}

TEST(resp_extra_bytes_ignored) {
    // Bytes after a complete response aren't consumed.
    http_resp_t p;
    http_resp_init(&p, false);
    const char *s = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhiEXTRA";
    const uint8_t *body;
    size_t body_len, at = 0, len = strlen(s);
    while (at < len) {
        size_t used = http_resp_feed(&p, (const uint8_t *)s + at, len - at, 100, &body, &body_len);
        if (!used)
            break;
        at += used;
    }
    CHECK_EQ(p.state, HTTP_P_DONE);
    CHECK_EQ(at, len - 5);
}

TEST(resp_chunked) {
    result_t *r = run_all("HTTP/1.1 200 OK\r\n"
                          "Transfer-Encoding: Chunked\r\n"
                          "\r\n"
                          "5\r\nhello\r\n"
                          "1;name=value\r\n \r\n"
                          "A \r\n0123456789\r\n"
                          "0\r\n"
                          "Trailer-Thing: x\r\n"
                          "\r\n", false, false);
    CHECK_EQ(r->p.state, HTTP_P_DONE);
    CHECK_EQ(r->body_len, 16);
    CHECK_MEM(r->body, "hello 0123456789", 16);
}

TEST(resp_bare_lf) {
    result_t *r = run_all("HTTP/1.1 200 OK\n"
                          "Transfer-Encoding: chunked\n"
                          "\n"
                          "3\nabc\n0\n\n", false, false);
    CHECK_EQ(r->p.state, HTTP_P_DONE);
    CHECK_EQ(r->body_len, 3);
}

TEST(resp_to_close) {
    // No length: the body runs to the end of the connection.
    result_t *r = run_all("HTTP/1.0 200 OK\r\n\r\nall of it", false, false);
    CHECK_EQ(r->p.state, HTTP_P_BODY_TO_CLOSE);
    CHECK_EQ(r->body_len, 9);
    r = run_all("HTTP/1.0 200 OK\r\n\r\nall of it", false, true);
    CHECK_EQ(r->p.state, HTTP_P_DONE);
}

TEST(resp_no_body) {
    result_t *r = run_all("HTTP/1.1 204 No Content\r\nContent-Length: 50\r\n\r\n", false, false);
    CHECK_EQ(r->p.state, HTTP_P_DONE);
    CHECK_EQ(r->body_len, 0);
    r = run_all("HTTP/1.1 304 Not Modified\r\n\r\n", false, false);
    CHECK_EQ(r->p.state, HTTP_P_DONE);
    r = run_all("HTTP/1.1 200 OK\r\nContent-Length: 1234\r\n\r\n", true, false);   // HEAD
    CHECK_EQ(r->p.state, HTTP_P_DONE);
    CHECK_EQ(r->body_len, 0);
    r = run_all("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", false, false);
    CHECK_EQ(r->p.state, HTTP_P_DONE);
}

TEST(resp_interim) {
    result_t *r = run_all("\r\nHTTP/1.1 100 Continue\r\nX-Interim: 1\r\n\r\n"
                          "HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok", false, false);
    CHECK_EQ(r->p.state, HTTP_P_DONE);
    CHECK_EQ(r->p.status, 201);
    CHECK(header_is(&r->p, "x-interim", NULL));
    CHECK(header_is(&r->p, "content-length", "2"));
    CHECK_EQ(r->body_len, 2);
}

TEST(resp_status_lines) {
    result_t *r = run_all("HTTP/1.1 404\r\n\r\n", false, true);
    CHECK_EQ(r->p.status, 404);
    CHECK(!strcmp(r->p.reason, ""));
    r = run_all("HTTP/1.1 500 \r\nContent-Length: 0\r\n\r\n", false, false);
    CHECK_EQ(r->p.status, 500);

    const char *bad[] = {
        "HTTP/2 200 OK\r\n\r\n", "HTTP/1.1 20 OK\r\n\r\n", "HTTP/1.1 2000 OK\r\n\r\n",
        "HTTP/1.1  200 OK\r\n\r\n", "http/1.1 200 OK\r\n\r\n", "HTTP/1.1 099 Low\r\n\r\n",
        "HTTP/1.1 600 High\r\n\r\n", "SSH-2.0-OpenSSH\r\n\r\n",
        "HTTP/1.1 101 Switching Protocols\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        r = run_all(bad[i], false, false);
        CHECK_EQ(r->p.state, HTTP_P_ERROR);
        CHECK(r->p.error != NULL);
    }
}

TEST(resp_framing_refused) {
    const char *bad[] = {
        // Both framings, conflicting or odd lengths.
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabc",
        "HTTP/1.1 200 OK\r\nContent-Length: 3, 3\r\n\r\nabc",
        "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: \r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1234567890123456\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
        // Malformed header lines.
        "HTTP/1.1 200 OK\r\nNo colon here\r\n\r\n",
        "HTTP/1.1 200 OK\r\n: no name\r\n\r\n",
        "HTTP/1.1 200 OK\r\nBad Name: x\r\n\r\n",
        "HTTP/1.1 200 OK\r\nA: 1\r\n folded\r\n\r\n",
        "HTTP/1.1 200 OK\r\nA: 1\rB: 2\r\n\r\n",
        "HTTP/1.1 200 OK\r\nA: \x1b[31m\r\n\r\n",
        // Bad chunks.
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n;ext\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5 5\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcX\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10000000\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\rx\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        result_t *r = run_all(bad[i], false, false);
        if (r->p.state != HTTP_P_ERROR)
            fprintf(stderr, "  accepted: case %zu\n", i);
        CHECK_EQ(r->p.state, HTTP_P_ERROR);
        CHECK(r->p.error != NULL);
    }
    // The largest allowed chunk size parses.
    result_t *r = run_all("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFF\r\n",
                          false, false);
    CHECK_EQ(r->p.state, HTTP_P_CHUNK_DATA);
    CHECK_EQ(r->p.remaining, 0xFFFFFFF);
}

TEST(resp_cut_short) {
    result_t *r = run_all("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhalf", false, true);
    CHECK_EQ(r->p.state, HTTP_P_ERROR);
    CHECK_EQ(r->body_len, 4);
    r = run_all("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n", false,
                true);
    CHECK_EQ(r->p.state, HTTP_P_ERROR);
    r = run_all("HTTP/1.1 200 OK\r\nContent-Le", false, true);
    CHECK_EQ(r->p.state, HTTP_P_ERROR);
    r = run_all("", false, true);
    CHECK_EQ(r->p.state, HTTP_P_ERROR);
}

TEST(resp_limits) {
    // Headers just within, and just over, HTTP_HEADERS_MAX.
    static char big[HTTP_HEADERS_MAX + 256];
    const char *head = "HTTP/1.1 200 OK\r\nX-Big: ";
    const char *tail = "\r\nContent-Length: 0\r\n\r\n";
    // The store holds "x-big:<value>\n" and "content-length:0\n"; the status line isn't kept.
    size_t value = HTTP_HEADERS_MAX - strlen("x-big:\n") - strlen("content-length:0\n") - 1;
    strcpy(big, head);
    memset(big + strlen(head), 'v', value);
    strcpy(big + strlen(head) + value, tail);
    result_t *r = run_all(big, false, false);
    CHECK_EQ(r->p.state, HTTP_P_DONE);
    char buf[HTTP_HEADERS_MAX];
    CHECK(http_resp_header(&r->p, "x-big", buf, sizeof(buf)) && strlen(buf) == value);
    CHECK(http_resp_header(&r->p, "x-big", buf, 10) == NULL);      // too small for the value

    memset(big + strlen(head), 'v', value + 2);
    strcpy(big + strlen(head) + value + 2, tail);
    r = run_all(big, false, false);
    CHECK_EQ(r->p.state, HTTP_P_ERROR);

    // A status line can't run on forever either.
    memset(big, 'H', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    r = run_all(big, false, false);
    CHECK_EQ(r->p.state, HTTP_P_ERROR);

    // Endless chunk extensions and trailers are cut off.
    static char ext[512];
    snprintf(ext, sizeof(ext), "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1;%0300d\r\n", 0);
    r = run_all(ext, false, false);
    CHECK_EQ(r->p.state, HTTP_P_ERROR);
    static char trailers[2048];
    int n = snprintf(trailers, sizeof(trailers),
                     "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n");
    for (int i = 0; i < 100; i++)
        n += snprintf(trailers + n, sizeof(trailers) - (size_t)n, "T%02d: xxxxxxxx\r\n", i);
    r = run_all(trailers, false, false);
    CHECK_EQ(r->p.state, HTTP_P_ERROR);
}

TEST(resp_max_body_zero) {
    // With no room for body bytes the parser stops at the body without consuming it.
    http_resp_t p;
    http_resp_init(&p, false);
    const char *s = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc";
    const uint8_t *body;
    size_t body_len;
    size_t used = http_resp_feed(&p, (const uint8_t *)s, strlen(s), 0, &body, &body_len);
    CHECK_EQ(used, strlen(s) - 3);
    CHECK(http_resp_headers_done(&p));
    CHECK_EQ(body_len, 0);
    used = http_resp_feed(&p, (const uint8_t *)s + used, 3, 0, &body, &body_len);
    CHECK_EQ(used, 0);
}

// Random bytes, and random corruptions of a valid response, must never crash or read out of
// bounds (the sanitizers watch), and must end in a defined state.
TEST(resp_fuzz) {
    const char *good = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nX: y\r\n\r\n"
                       "4\r\nwiki\r\n5;e=1\r\npedia\r\n0\r\nT: 1\r\n\r\n";
    size_t len = strlen(good);
    static char buf[256];
    unsigned seed = 12345;
    for (int iter = 0; iter < 20000; iter++) {
        memcpy(buf, good, len + 1);
        int flips = 1 + iter % 4;
        for (int f = 0; f < flips; f++) {
            seed = seed * 1103515245u + 12345u;
            size_t at = (seed >> 8) % len;
            seed = seed * 1103515245u + 12345u;
            buf[at] = (char)(seed >> 16);
        }
        result_t *r = &results[0];
        // buf may now contain NULs; feed len bytes regardless.
        run(r, buf, len, false, (size_t)(1 + iter % 9), (size_t)(1 + iter % 5), iter % 2);
        CHECK(r->p.state <= HTTP_P_ERROR);
        CHECK(r->body_len <= len);
    }
}

TEST_MAIN(
    RUN(url_basic);
    RUN(url_refused);
    RUN(request_get);
    RUN(request_post);
    RUN(request_refused);
    RUN(resp_content_length);
    RUN(resp_extra_bytes_ignored);
    RUN(resp_chunked);
    RUN(resp_bare_lf);
    RUN(resp_to_close);
    RUN(resp_no_body);
    RUN(resp_interim);
    RUN(resp_status_lines);
    RUN(resp_framing_refused);
    RUN(resp_cut_short);
    RUN(resp_limits);
    RUN(resp_max_body_zero);
    RUN(resp_fuzz);
)
