// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// src/net/http_msg.c: what an HTTP server sends back (net/http.c feeds it as TCP delivers it),
// and URLs as typed. The first bytes choose HEAD or not, the body limit and how the response
// is split into pieces; the rest is the response.

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz.h"
#include "net/http_msg.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // The same bytes as a URL (NUL-terminated copy, as the shell passes it).
    char *url = malloc(size + 1);
    if (!url)
        return 0;
    memcpy(url, data, size);
    url[size] = '\0';
    http_url_t u;
    http_url_parse(url, &u);
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
