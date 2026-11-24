// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// HTTP/1.1 messages for the HTTPS client (net/http.c): URL parsing, request building and an
// incremental response parser. Pure C (no SDK, no heap), so it's unit tested on the host.
//
// The parser takes the response bytes in whatever pieces they arrive and hands back the body
// as slices of that input, without copying: plain (Content-Length), chunked, or up to the end
// of the connection. It stores the headers (lower-case names, trimmed values) for lookup.
// Everything a server sends is bounded: header size, line length, chunk size.

#ifndef SOS_HTTP_MSG_H
#define SOS_HTTP_MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HTTP_HOST_MAX     63        // as tls_open() takes
#define HTTP_PATH_MAX     511       // path and query
#define HTTP_HEADERS_MAX  4096      // response status line and headers, together
#define HTTP_REASON_MAX   63

// --- URLs ----------------------------------------------------------------------------------------

typedef struct {
    char host[HTTP_HOST_MAX + 1];
    uint16_t port;                  // 443 unless given
    char path[HTTP_PATH_MAX + 1];   // starts with '/', includes the query; no fragment
} http_url_t;

// Parse "https://host[:port][/path][?query][#fragment]". Only https; no user:password@, no IPv6
// literals. The path must already be percent-encoded: printable ASCII without spaces.
// Returns NULL, or what's wrong.
const char *http_url_parse(const char *url, http_url_t *out);

// --- requests ------------------------------------------------------------------------------------

typedef struct {
    const char *method;             // upper-case token: "GET", "POST", ...
    const http_url_t *url;
    const char *content_type;       // NULL: no Content-Type header
    const char *const *headers;     // extra "Name: value" lines, NULL-terminated; may be NULL
    size_t body_len;                // sent as Content-Length when non-zero, or for POST/PUT/PATCH
} http_request_t;

// Write the request line and headers (ending with the blank line) into buf. The client always
// sends Host, User-Agent, Accept-Encoding: identity (no compression) and Connection: close.
// Extra headers can't set those, Content-Length or Transfer-Encoding, and can't contain CR, LF
// or other control characters (no header injection). Returns the length, or 0 with *err set.
size_t http_request_build(const http_request_t *req, char *buf, size_t size, const char **err);

// --- responses -----------------------------------------------------------------------------------

typedef enum {
    HTTP_P_STATUS,          // reading the status line
    HTTP_P_HEADERS,         // reading header lines
    HTTP_P_BODY,            // Content-Length body
    HTTP_P_BODY_TO_CLOSE,   // body runs to the end of the connection
    HTTP_P_CHUNK_SIZE,      // chunked: a chunk-size line
    HTTP_P_CHUNK_DATA,
    HTTP_P_CHUNK_END,       // the CRLF after a chunk's data
    HTTP_P_TRAILERS,        // after the last chunk: trailer lines up to a blank line
    HTTP_P_DONE,            // the whole response is in; later bytes are ignored
    HTTP_P_ERROR,           // see http_resp_t.error
} http_parse_state_t;

typedef struct {
    http_parse_state_t state;
    bool head;                      // response to HEAD: no body whatever the headers say
    int status;                     // e.g. 200, once the headers are in
    int minor;                      // HTTP/1.<minor>
    char reason[HTTP_REASON_MAX + 1];
    bool chunked;
    bool has_length;
    uint64_t length;                // Content-Length, when has_length
    uint64_t remaining;             // body or chunk bytes still to come
    uint64_t body_received;
    const char *error;

    // Header store: entries "name:value\n" (name lower-case, value trimmed). Also the line
    // buffer while a line is being read.
    char hdr[HTTP_HEADERS_MAX];
    size_t hdr_len;                 // stored entries
    size_t line_len;                // the line being read, at hdr + hdr_len
    bool line_cr;                   // last byte of the line so far was CR
    unsigned chunk_line;            // length of the chunk-size or trailer line so far
    uint32_t chunk_size;
    bool chunk_digits, chunk_ext, chunk_ws;
    size_t trailer_bytes;
} http_resp_t;

// Start parsing a response. head: the request was HEAD.
void http_resp_init(http_resp_t *p, bool head);

// Feed len bytes of the response. Returns how many were consumed; the caller feeds the rest
// again. When body bytes are found, *body points at them (inside in) and *body_len says how
// many, at most max_body; the call returns right after them. Otherwise *body_len is 0. Stops
// consuming at HTTP_P_DONE or HTTP_P_ERROR.
size_t http_resp_feed(http_resp_t *p, const uint8_t *in, size_t len, size_t max_body,
                      const uint8_t **body, size_t *body_len);

// The connection ended (cleanly). True if that completes the response; otherwise the response
// was cut short and the parser moves to HTTP_P_ERROR.
bool http_resp_eof(http_resp_t *p);

// True once the status line and all headers are in (state past HTTP_P_HEADERS).
bool http_resp_headers_done(const http_resp_t *p);

// The value of header name (case-insensitive), or NULL. With repeated headers, the first.
// Writes into buf, which must hold the value.
const char *http_resp_header(const http_resp_t *p, const char *name, char *buf, size_t size);

#endif
