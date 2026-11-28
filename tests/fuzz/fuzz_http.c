// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// src/net/http_msg.c: what an HTTP server sends back (net/http.c feeds it as TCP delivers it),
// and URLs as typed. The first bytes choose HEAD or not, the body limit and how the response
// is split into pieces; the rest is the response. The same bytes are also a redirect's Location
// (resolved against a fixed URL and against themselves as a URL) and a URL path whose last
// segment names a download (fetch), checked as a file name (files/path.c).

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "files/path.h"
#include "fuzz.h"
#include "net/http_msg.h"

static char text[HTTP_URL_MAX + 1];
static char name[HTTP_PATH_MAX + 1];

// Follow `loc` from `base` as http.c does; the result, when taken, must be a URL that parses.
static void redirect(const http_url_t *base, const char *loc, bool downgrade) {
    http_url_t u = *base;
    if (http_url_resolve(base, loc, text, sizeof(text)) == NULL && strlen(text) > HTTP_URL_MAX)
        abort();
    if (http_redirect_target(&u, loc, downgrade, text, sizeof(text)) == NULL) {
        http_url_t again;
        if (http_url_parse(text, &again) || strcmp(again.host, u.host) ||
            strcmp(again.path, u.path) || again.port != u.port)
            abort();
    }
}

// The name a path gives, and whether it may be a file name.
static void url_name(const char *path) {
    if (http_url_name(path, name, sizeof(name)) == NULL)
        path_name_check(name);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // The same bytes as a URL (NUL-terminated copy, as the shell passes it).
    char *url = malloc(size + 1);
    if (!url)
        return 0;
    memcpy(url, data, size);
    url[size] = '\0';
    http_url_t u, base;
    bool url_ok = http_url_parse(url, &u) == NULL;
    if (url_ok)
        url_name(u.path);
    url_name(url);
    if (http_url_parse("https://a.example:8443/d/e/f?q=1", &base) == NULL) {
        redirect(&base, url, false);
        redirect(&base, url, true);
    }
    if (url_ok)
        redirect(&u, "../x/./y?z#f", false);
    free(url);

    uint8_t flags = fuzz_byte(&data, &size);
    size_t max_body = fuzz_byte(&data, &size);          // 0: headers only, as http.c starts
    size_t piece = (size_t)fuzz_byte(&data, &size) + 1;  // TCP segment size
    http_resp_t p;
    http_resp_init(&p, flags & 1);
    while (size) {
        size_t n = size < piece ? size : piece;
        const uint8_t *body;
        size_t body_len;
        size_t used = http_resp_feed(&p, data, n, max_body, &body, &body_len);
        if (body_len && (body < data || body + body_len > data + n))
            abort();                                    // body must point inside the input
        if (used > n)
            abort();
        if (!used && !body_len)
            break;                                      // done, error, or nothing taken
        data += used;
        size -= used;
    }
    if (flags & 2)
        http_resp_eof(&p);
    if (http_resp_headers_done(&p)) {
        char buf[256];
        http_resp_header(&p, "content-length", buf, sizeof(buf));
        http_resp_header(&p, "location", buf, sizeof(buf));
    }
    return 0;
}
