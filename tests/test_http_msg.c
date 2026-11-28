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

TEST(url_format) {
    http_url_t u;
    char out[HTTP_URL_MAX + 1];
    CHECK(http_url_parse("HTTPS://Example.COM/a?b=1#frag", &u) == NULL);
    CHECK(http_url_format(&u, out, sizeof(out)) == strlen("https://example.com/a?b=1"));
    CHECK(!strcmp(out, "https://example.com/a?b=1"));
    CHECK(http_url_parse("https://h:8443", &u) == NULL);
    http_url_format(&u, out, sizeof(out));
    CHECK(!strcmp(out, "https://h:8443/"));
    CHECK_EQ(http_url_format(&u, out, 5), 0);       // doesn't fit
    CHECK(!strcmp(out, ""));

    // The longest URL the parser takes fits HTTP_URL_MAX, and parses back the same.
    static char longest[HTTP_URL_MAX + 16];
    char *p = longest + sprintf(longest, "https://");
    memset(p, 'h', HTTP_HOST_MAX);
    p += HTTP_HOST_MAX;
    p += sprintf(p, ":65535/");
    memset(p, 'p', HTTP_PATH_MAX - 1);
    p[HTTP_PATH_MAX - 1] = '\0';
    CHECK(http_url_parse(longest, &u) == NULL);
    CHECK_EQ(http_url_format(&u, out, sizeof(out)), HTTP_URL_MAX);
    CHECK(!strcmp(out, longest));
}

// --- redirects -----------------------------------------------------------------------------------

static char resolved[HTTP_URL_MAX + 1];

// Resolve loc against base; NULL if refused, else the result.
static const char *resolve(const char *base, const char *loc) {
    http_url_t u;
    if (http_url_parse(base, &u))
        return "(bad base)";
    return http_url_resolve(&u, loc, resolved, sizeof(resolved)) ? NULL : resolved;
}

#define CHECK_RESOLVE(base, loc, want) do {                                                 \
        const char *got_ = resolve((base), (loc));                                          \
        test_checks++;                                                                      \
        if (!got_ || strcmp(got_, (want))) {                                                \
            test_failures++;                                                                \
            fprintf(stderr, "%s:%d: resolve(%s, %s) = %s, expected %s\n", __FILE__, __LINE__, \
                    (base), (loc), got_ ? got_ : "(refused)", (want));                      \
        }                                                                                   \
    } while (0)

TEST(redirect_statuses) {
    static const int yes[] = {301, 302, 303, 307, 308};
    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++)
        CHECK(http_status_redirects(yes[i]));
    static const int no[] = {200, 204, 300, 304, 305, 306, 309, 400, 404, 500};
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++)
        CHECK(!http_status_redirects(no[i]));
}

TEST(resolve_rfc3986) {
    // RFC 3986 §5.4.1's normal examples, with its base on https.
    const char *b = "https://a/b/c/d;p?q";
    CHECK_RESOLVE(b, "g", "https://a/b/c/g");
    CHECK_RESOLVE(b, "./g", "https://a/b/c/g");
    CHECK_RESOLVE(b, "g/", "https://a/b/c/g/");
    CHECK_RESOLVE(b, "/g", "https://a/g");
    CHECK_RESOLVE(b, "//g", "https://g");
    CHECK_RESOLVE(b, "?y", "https://a/b/c/d;p?y");
    CHECK_RESOLVE(b, "g?y", "https://a/b/c/g?y");
    CHECK_RESOLVE(b, "#s", "https://a/b/c/d;p?q");      // a fragment alone: the same resource
    CHECK_RESOLVE(b, "g#s", "https://a/b/c/g");
    CHECK_RESOLVE(b, "g?y#s", "https://a/b/c/g?y");
    CHECK_RESOLVE(b, ";x", "https://a/b/c/;x");
    CHECK_RESOLVE(b, "g;x", "https://a/b/c/g;x");
    CHECK_RESOLVE(b, ".", "https://a/b/c/");
    CHECK_RESOLVE(b, "./", "https://a/b/c/");
    CHECK_RESOLVE(b, "..", "https://a/b/");
    CHECK_RESOLVE(b, "../", "https://a/b/");
    CHECK_RESOLVE(b, "../g", "https://a/b/g");
    CHECK_RESOLVE(b, "../..", "https://a/");
    CHECK_RESOLVE(b, "../../", "https://a/");
    CHECK_RESOLVE(b, "../../g", "https://a/g");
    // ... and abnormal ones (§5.4.2).
    CHECK_RESOLVE(b, "../../../g", "https://a/g");
    CHECK_RESOLVE(b, "../../../../g", "https://a/g");
    CHECK_RESOLVE(b, "/./g", "https://a/g");
    CHECK_RESOLVE(b, "/../g", "https://a/g");
    CHECK_RESOLVE(b, "g.", "https://a/b/c/g.");
    CHECK_RESOLVE(b, ".g", "https://a/b/c/.g");
    CHECK_RESOLVE(b, "g..", "https://a/b/c/g..");
    CHECK_RESOLVE(b, "..g", "https://a/b/c/..g");
    CHECK_RESOLVE(b, "./../g", "https://a/b/g");
    CHECK_RESOLVE(b, "./g/.", "https://a/b/c/g/");
    CHECK_RESOLVE(b, "g/./h", "https://a/b/c/g/h");
    CHECK_RESOLVE(b, "g/../h", "https://a/b/c/h");
    CHECK_RESOLVE(b, "g;x=1/./y", "https://a/b/c/g;x=1/y");
    CHECK_RESOLVE(b, "g;x=1/../y", "https://a/b/c/y");
    CHECK_RESOLVE(b, "g?y/./x", "https://a/b/c/g?y/./x");   // dots in a query stay
    CHECK_RESOLVE(b, "g?y/../x", "https://a/b/c/g?y/../x");
}

TEST(resolve_absolute_and_ports) {
    CHECK_RESOLVE("https://a/x", "https://b.example/f.tgz", "https://b.example/f.tgz");
    CHECK_RESOLVE("https://a/x", "HTTPS://B/f", "HTTPS://B/f");     // as given; parsed later
    CHECK_RESOLVE("https://a/x", "http://b/f", "http://b/f");
    CHECK_RESOLVE("https://a:8443/x/y", "z", "https://a:8443/x/z");
    CHECK_RESOLVE("https://a:8443/x/y", "/z?q=1", "https://a:8443/z?q=1");
    CHECK_RESOLVE("https://a:8443/x/y", "//b/z", "https://b/z");
    CHECK_RESOLVE("https://a/", "f", "https://a/f");
    CHECK_RESOLVE("https://a", "../../f", "https://a/f");
    CHECK_RESOLVE("https://a/d/?q=1", "f", "https://a/d/f");
    CHECK_RESOLVE("https://a/d/f?q=/x/y", "g", "https://a/d/g");     // '/' in the base's query
}

TEST(resolve_refused) {
    http_url_t u;
    CHECK(http_url_parse("https://a/b", &u) == NULL);
    char out[HTTP_URL_MAX + 1];
    CHECK(http_url_resolve(&u, "", out, sizeof(out)) != NULL);
    CHECK(http_url_resolve(&u, "a b", out, sizeof(out)) != NULL);
    CHECK(http_url_resolve(&u, "/a\r\nX: y", out, sizeof(out)) != NULL);
    CHECK(http_url_resolve(&u, "/caf\xc3\xa9", out, sizeof(out)) != NULL);
    CHECK(http_url_resolve(&u, "/\x7f", out, sizeof(out)) != NULL);
    CHECK(http_url_resolve(&u, "/x", out, 8) != NULL);      // doesn't fit out

    // Longer than HTTP_URL_MAX: refused, whatever out holds. Exactly the limit: taken.
    static char loc[HTTP_URL_MAX + 64], big[HTTP_URL_MAX + 64];
    strcpy(loc, "https://h/");
    memset(loc + 10, 'p', HTTP_URL_MAX - 10);
    loc[HTTP_URL_MAX] = '\0';
    CHECK(http_url_resolve(&u, loc, big, sizeof(big)) == NULL);
    CHECK_EQ(strlen(big), HTTP_URL_MAX);
    loc[HTTP_URL_MAX] = 'p';
    loc[HTTP_URL_MAX + 1] = '\0';
    CHECK(http_url_resolve(&u, loc, big, sizeof(big)) != NULL);
    // A long fragment doesn't count: it isn't kept.
    strcpy(loc, "/f#");
    memset(loc + 3, 'x', HTTP_URL_MAX);
    loc[HTTP_URL_MAX + 3] = '\0';
    CHECK(http_url_resolve(&u, loc, big, sizeof(big)) == NULL);
    CHECK(!strcmp(big, "https://a/f"));
}

TEST(redirect_target) {
    http_url_t u;
    char text[HTTP_URL_MAX + 1];

    CHECK(http_url_parse("https://a/dir/file", &u) == NULL);
    CHECK(http_redirect_target(&u, "../other/f.tgz?x=1", false, text, sizeof(text)) == NULL);
    CHECK(!strcmp(text, "https://a/other/f.tgz?x=1"));
    CHECK(!strcmp(u.host, "a"));
    CHECK(!strcmp(u.path, "/other/f.tgz?x=1"));

    CHECK(http_redirect_target(&u, "https://cdn.example:444/x", false, text, sizeof(text)) ==
          NULL);
    CHECK(!strcmp(u.host, "cdn.example"));
    CHECK_EQ(u.port, 444);

    // Down to plain http: refused, with -k or without (plain http isn't there yet); the
    // messages differ.
    CHECK(http_url_parse("https://a/f", &u) == NULL);
    const char *no_k = http_redirect_target(&u, "http://a/f", false, text, sizeof(text));
    CHECK(http_url_parse("https://a/f", &u) == NULL);
    const char *with_k = http_redirect_target(&u, "HTTP://a/f", true, text, sizeof(text));
    CHECK(no_k && with_k && strcmp(no_k, with_k));
    CHECK(strstr(no_k, "-k") != NULL);

    static const char *const refused[] = {
        "ftp://a/f", "mailto:x@y", "https://user:pw@a/f", "//user@a/f", "https://[::1]/f",
        "https://a:0/f", "https://a_b/f", "javascript:alert(1)", "https:", "https://",
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        CHECK(http_url_parse("https://a/f", &u) == NULL);
        CHECK(http_redirect_target(&u, refused[i], false, text, sizeof(text)) != NULL);
    }
    // A target whose path is over the parser's limit (the whole fits HTTP_URL_MAX).
    static char loc[HTTP_URL_MAX + 1];
    strcpy(loc, "/");
    memset(loc + 1, 'p', HTTP_PATH_MAX);
    loc[HTTP_PATH_MAX + 1] = '\0';
    CHECK(http_url_parse("https://a/f", &u) == NULL);
    CHECK(http_redirect_target(&u, loc, false, text, sizeof(text)) != NULL);
}

TEST(url_name) {
    char name[64];
    CHECK(http_url_name("/pkg/pkg-1.2.tar.gz", name, sizeof(name)) == NULL);
    CHECK(!strcmp(name, "pkg-1.2.tar.gz"));
    CHECK(http_url_name("/a/SHA256SUMS?raw=1&x=/y", name, sizeof(name)) == NULL);
    CHECK(!strcmp(name, "SHA256SUMS"));
    CHECK(http_url_name("/my%20file%2Etxt", name, sizeof(name)) == NULL);
    CHECK(!strcmp(name, "my file.txt"));
    CHECK(http_url_name("/caf%C3%a9", name, sizeof(name)) == NULL);
    CHECK(!strcmp(name, "caf\xc3\xa9"));
    CHECK(http_url_name("/a%2Fb", name, sizeof(name)) == NULL);   // decoded; path_name_check()
    CHECK(!strcmp(name, "a/b"));                                    // refuses it

    CHECK(http_url_name("/", name, sizeof(name)) != NULL);
    CHECK(http_url_name("/dir/", name, sizeof(name)) != NULL);
    CHECK(http_url_name("/?q=1", name, sizeof(name)) != NULL);
    CHECK(http_url_name("/a%", name, sizeof(name)) != NULL);
    CHECK(http_url_name("/a%4", name, sizeof(name)) != NULL);
    CHECK(http_url_name("/a%4?", name, sizeof(name)) != NULL);
    CHECK(http_url_name("/a%zz", name, sizeof(name)) != NULL);
    CHECK(http_url_name("/a%00b", name, sizeof(name)) != NULL);
    CHECK(http_url_name("/0123456789", name, 10) != NULL);         // doesn't fit
    CHECK(http_url_name("/012345678", name, 10) == NULL);          // exactly
}

TEST(resp_has_header) {
    http_resp_t p;
    http_resp_init(&p, false);
    static char resp[2048];
    strcpy(resp, "HTTP/1.1 302 Found\r\nLocation: ");
    size_t at = strlen(resp);
    memset(resp + at, 'x', 1500);
    strcpy(resp + at + 1500, "\r\nContent-Length: 0\r\n\r\n");
    const uint8_t *body;
    size_t body_len;
    http_resp_feed(&p, (const uint8_t *)resp, strlen(resp), 0, &body, &body_len);
    CHECK(http_resp_headers_done(&p));
    char small[16];
    CHECK(http_resp_header(&p, "location", small, sizeof(small)) == NULL);  // too long for it
    CHECK(http_resp_has_header(&p, "LOCATION"));
    CHECK(!http_resp_has_header(&p, "locatio"));
    CHECK(!http_resp_has_header(&p, "x-none"));
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
    RUN(url_format);
    RUN(redirect_statuses);
    RUN(resolve_rfc3986);
    RUN(resolve_absolute_and_ports);
    RUN(resolve_refused);
    RUN(redirect_target);
    RUN(url_name);
    RUN(resp_has_header);
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
