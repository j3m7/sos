// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "net/http_msg.h"

#include <stdio.h>
#include <string.h>

#define CHUNK_LINE_MAX   128        // a chunk-size line, extensions included
#define TRAILERS_MAX     1024       // all trailer lines together (read and dropped)
#define CHUNK_SIZE_LIMIT 0x10000000u    // 256 MB: a larger chunk is refused

// --- small helpers -------------------------------------------------------------------------------

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int hex_value(char c) {
    if (is_digit(c))
        return c - '0';
    c = lower(c);
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

// Case-insensitive comparison of n bytes of a with the NUL-terminated b.
static bool ieq(const char *a, size_t n, const char *b) {
    if (strlen(b) != n)
        return false;
    for (size_t i = 0; i < n; i++)
        if (lower(a[i]) != lower(b[i]))
            return false;
    return true;
}

// RFC 9110 token characters (header names, methods).
static bool is_tchar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) ||
           (c && strchr("!#$%&'*+-.^_`|~", c));
}

// Bounded output into a buffer, always NUL-terminated; overflow is sticky.
typedef struct {
    char *buf;
    size_t size, len;
    bool overflow;
} out_t;

static void put(out_t *o, const char *s, size_t n) {
    if (o->overflow || o->len + n >= o->size) {
        o->overflow = true;
        return;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
}

static void puts_(out_t *o, const char *s) {
    put(o, s, strlen(s));
}

// --- URLs ----------------------------------------------------------------------------------------

const char *http_url_parse(const char *url, http_url_t *out) {
    memset(out, 0, sizeof(*out));
    if (!url || !*url)
        return "no URL";
    if (!ieq(url, strlen(url) < 8 ? strlen(url) : 8, "https://")) {
        if (strlen(url) >= 7 && ieq(url, 7, "http://"))
            return "plain http isn't supported: use https://";
        return "the URL must start with https://";
    }
    const char *p = url + 8;
    const char *end = p + strcspn(p, "/?#");        // end of host[:port]

    if (memchr(p, '@', (size_t)(end - p)))
        return "user names in URLs aren't supported";
    if (*p == '[')
        return "IPv6 addresses aren't supported";
    const char *colon = memchr(p, ':', (size_t)(end - p));
    const char *host_end = colon ? colon : end;
    size_t host_len = (size_t)(host_end - p);
    if (host_len == 0)
        return "no host name";
    if (host_len > HTTP_HOST_MAX)
        return "host name too long";
    for (size_t i = 0; i < host_len; i++) {
        char c = p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '-' ||
              c == '.'))
            return "bad character in the host name";
        out->host[i] = lower(c);
    }

    out->port = 443;
    if (colon) {
        const char *d = colon + 1;
        size_t n = (size_t)(end - d);
        if (n == 0 || n > 5)
            return "bad port";
        unsigned long port = 0;
        for (size_t i = 0; i < n; i++) {
            if (!is_digit(d[i]))
                return "bad port";
            port = port * 10 + (unsigned long)(d[i] - '0');
        }
        if (port == 0 || port > 65535)
            return "bad port";
        out->port = (uint16_t)port;
    }

    // Path and query, up to any fragment. An empty path is "/".
    size_t path_len = strcspn(end, "#");
    size_t at = 0;
    if (*end != '/')
        out->path[at++] = '/';
    if (at + path_len > HTTP_PATH_MAX)
        return "URL path too long";
    for (size_t i = 0; i < path_len; i++) {
        char c = end[i];
        if (c <= ' ' || c == 0x7f || (unsigned char)c >= 0x80)
            return "URL path has spaces or other characters that must be percent-encoded";
        out->path[at++] = c;
    }
    out->path[at] = '\0';
    return NULL;
}

size_t http_url_format(const http_url_t *u, char *out, size_t size) {
    int n = u->port == 443 ? snprintf(out, size, "https://%s%s", u->host, u->path)
                           : snprintf(out, size, "https://%s:%u%s", u->host, (unsigned)u->port,
                                      u->path);
    if (n < 0 || (size_t)n >= size) {
        if (size)
            out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

// --- redirects -----------------------------------------------------------------------------------

bool http_status_redirects(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Length of a URI scheme at the start of s, followed by ':' ("https:" gives 5), or 0 if s
// doesn't start with one (RFC 3986: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":").
static size_t scheme_len(const char *s, size_t n) {
    if (n == 0 || !is_alpha(s[0]))
        return 0;
    for (size_t i = 1; i < n; i++) {
        char c = s[i];
        if (c == ':')
            return i;
        if (!is_alpha(c) && !is_digit(c) && c != '+' && c != '-' && c != '.')
            return 0;
    }
    return 0;
}

// Remove "." and ".." segments (RFC 3986 §5.2.4) from the path s[0..n), which starts with '/',
// in place. Returns the new length (at least 1: "/"). The output never runs ahead of the input,
// so one buffer does.
static size_t remove_dots(char *s, size_t n) {
    size_t r = 0, w = 0;
    while (r < n) {                             // s[r] == '/'
        size_t start = r + 1, end = start;
        while (end < n && s[end] != '/')
            end++;
        size_t len = end - start;
        bool last = end >= n;
        if (len == 1 && s[start] == '.') {
            if (last)
                s[w++] = '/';
        } else if (len == 2 && s[start] == '.' && s[start + 1] == '.') {
            while (w > 0 && s[w - 1] != '/')
                w--;
            if (w > 0)
                w--;                            // the '/' before the dropped segment
            if (last)
                s[w++] = '/';
        } else {
            s[w++] = '/';
            memmove(s + w, s + start, len);
            w += len;
        }
        r = end;
    }
    if (w == 0)
        s[w++] = '/';
    return w;
}

const char *http_url_resolve(const http_url_t *base, const char *location, char *out,
                             size_t size) {
    if (size)
        out[0] = '\0';
    size_t n = strcspn(location, "#");          // a fragment isn't sent
    if (n == 0 && location[0] != '#')
        return "empty Location";
    if (n == 0) {                               // a fragment alone: the same resource
        if (size > HTTP_URL_MAX + 1)
            size = HTTP_URL_MAX + 1;
        return http_url_format(base, out, size) ? NULL : "Location too long";
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)location[i];
        if (c <= ' ' || c >= 0x7f)
            return "Location has spaces or other characters that must be percent-encoded";
    }
    size_t limit = size < HTTP_URL_MAX + 1 ? size : HTTP_URL_MAX + 1;
    out_t o = {out, limit, 0, limit == 0};

    if (scheme_len(location, n)) {
        put(&o, location, n);                   // absolute: as given
    } else if (n >= 2 && location[0] == '/' && location[1] == '/') {
        puts_(&o, "https:");                    // same scheme, another host
        put(&o, location, n);
    } else {
        // Same origin. The path and the query are joined in out, then the dots removed.
        char port[8] = "";
        if (base->port != 443)
            snprintf(port, sizeof(port), ":%u", (unsigned)base->port);
        puts_(&o, "https://");
        puts_(&o, base->host);
        puts_(&o, port);
        size_t path_at = o.len;
        size_t bpath = strcspn(base->path, "?");
        if (location[0] == '/') {
            put(&o, location, n);
        } else if (location[0] == '?') {
            put(&o, base->path, bpath);
            put(&o, location, n);
        } else {
            // A relative path: the base's directory (up to its last '/'), then the Location.
            size_t dir = bpath;
            while (dir > 0 && base->path[dir - 1] != '/')
                dir--;
            put(&o, base->path, dir);
            put(&o, location, n);
        }
        if (!o.overflow && location[0] != '?') {
            size_t plen = strcspn(out + path_at, "?");     // the path, without the query
            size_t new_len = remove_dots(out + path_at, plen);
            memmove(out + path_at + new_len, out + path_at + plen, o.len - path_at - plen + 1);
            o.len -= plen - new_len;
        }
    }
    if (o.overflow)
        return "Location too long";
    return NULL;
}

const char *http_redirect_target(http_url_t *url, const char *location, bool allow_downgrade,
                                 char *text, size_t size) {
    const char *err = http_url_resolve(url, location, text, size);
    if (err)
        return err;
    size_t len = strlen(text);
    if (len >= 7 && ieq(text, 7, "http://"))
        return allow_downgrade ? "plain http isn't supported yet"
                               : "from https to plain http (refused without -k)";
    if (len < 8 || !ieq(text, 8, "https://"))
        return "not an https:// URL";
    return http_url_parse(text, url);
}

const char *http_url_name(const char *path, char *out, size_t size) {
    size_t end = strcspn(path, "?");
    size_t start = end;
    while (start > 0 && path[start - 1] != '/')
        start--;
    if (start == end)
        return "the URL names no file";
    size_t w = 0;
    for (size_t i = start; i < end; i++) {
        char c = path[i];
        if (c == '%') {
            int hi = i + 2 < end ? hex_value(path[i + 1]) : -1;
            int lo = hi >= 0 ? hex_value(path[i + 2]) : -1;
            if (lo < 0)
                return "bad percent-encoding in the URL";
            c = (char)(hi * 16 + lo);
            if (c == '\0')
                return "the URL's name holds a NUL";
            i += 2;
        }
        if (w + 1 >= size)
            return "the URL's name is too long";
        out[w++] = c;
    }
    out[w] = '\0';
    return NULL;
}

// --- requests ------------------------------------------------------------------------------------

// Header names the client sets itself, or that would change how the message is framed.
static bool reserved_header(const char *name, size_t n) {
    static const char *const names[] = {
        "host", "content-length", "transfer-encoding", "connection", "accept-encoding",
        "content-type", "te", "trailer", "upgrade", "keep-alive", "proxy-connection",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (ieq(name, n, names[i]))
            return true;
    return false;
}

// Check one extra header line "Name: value". Returns NULL or what's wrong.
static const char *check_header(const char *h, size_t *name_len) {
    const char *colon = strchr(h, ':');
    if (!colon || colon == h)
        return "extra header must be \"Name: value\"";
    for (const char *c = h; c < colon; c++)
        if (!is_tchar(*c))
            return "bad character in an extra header's name";
    for (const char *c = colon + 1; *c; c++)
        if (((unsigned char)*c < 0x20 && *c != '\t') || *c == 0x7f)
            return "control character in an extra header";
    *name_len = (size_t)(colon - h);
    if (reserved_header(h, *name_len))
        return "that header is set by the client";
    return NULL;
}

static bool method_has_body(const char *m) {
    return !strcmp(m, "POST") || !strcmp(m, "PUT") || !strcmp(m, "PATCH");
}

size_t http_request_build(const http_request_t *req, char *buf, size_t size, const char **err) {
    *err = NULL;
    const char *m = req->method;
    size_t mlen = m ? strlen(m) : 0;
    if (mlen == 0 || mlen > 15) {
        *err = "bad method";
        return 0;
    }
    for (size_t i = 0; i < mlen; i++)
        if (m[i] < 'A' || m[i] > 'Z') {
            *err = "bad method";
            return 0;
        }
    if (req->content_type) {
        for (const char *c = req->content_type; *c; c++)
            if ((unsigned char)*c < 0x20 || *c == 0x7f) {
                *err = "control character in the content type";
                return 0;
            }
    }

    bool own_agent = false;
    for (const char *const *h = req->headers; h && *h; h++) {
        size_t n;
        const char *e = check_header(*h, &n);
        if (e) {
            *err = e;
            return 0;
        }
        if (ieq(*h, n, "user-agent"))
            own_agent = true;
    }

    out_t o = {buf, size, 0, size == 0};
    if (size)
        buf[0] = '\0';
    char num[32];
    puts_(&o, m);
    puts_(&o, " ");
    puts_(&o, req->url->path);
    puts_(&o, " HTTP/1.1\r\nHost: ");
    puts_(&o, req->url->host);
    if (req->url->port != 443) {
        snprintf(num, sizeof(num), ":%u", (unsigned)req->url->port);
        puts_(&o, num);
    }
    puts_(&o, "\r\n");
    if (!own_agent)
        puts_(&o, "User-Agent: sos\r\n");
    puts_(&o, "Accept-Encoding: identity\r\nConnection: close\r\n");
    if (req->content_type) {
        puts_(&o, "Content-Type: ");
        puts_(&o, req->content_type);
        puts_(&o, "\r\n");
    }
    if (req->body_len || method_has_body(m)) {
        snprintf(num, sizeof(num), "Content-Length: %zu\r\n", req->body_len);
        puts_(&o, num);
    }
    for (const char *const *h = req->headers; h && *h; h++) {
        puts_(&o, *h);
        puts_(&o, "\r\n");
    }
    puts_(&o, "\r\n");
    if (o.overflow) {
        *err = "request headers too long";
        return 0;
    }
    return o.len;
}

// --- responses -----------------------------------------------------------------------------------

static void set_error(http_resp_t *p, const char *e) {
    p->state = HTTP_P_ERROR;
    p->error = e;
}

void http_resp_init(http_resp_t *p, bool head) {
    memset(p, 0, sizeof(*p));
    p->state = HTTP_P_STATUS;
    p->head = head;
}

bool http_resp_headers_done(const http_resp_t *p) {
    return p->state > HTTP_P_HEADERS && p->state != HTTP_P_ERROR;
}

static void status_line(http_resp_t *p, const char *s, size_t n) {
    if (n == 0)
        return;                                 // empty lines before the status line: ignore
    // "HTTP/1.x 200[ reason]"
    if (n < 12 || memcmp(s, "HTTP/1.", 7) || !is_digit(s[7]) || s[8] != ' ' ||
        !is_digit(s[9]) || !is_digit(s[10]) || !is_digit(s[11]) || (n > 12 && s[12] != ' ')) {
        set_error(p, "bad status line");
        return;
    }
    p->minor = s[7] - '0';
    p->status = (s[9] - '0') * 100 + (s[10] - '0') * 10 + (s[11] - '0');
    if (p->status < 100 || p->status > 599) {
        set_error(p, "bad status code");
        return;
    }
    size_t rlen = n > 13 ? n - 13 : 0;
    if (rlen > HTTP_REASON_MAX)
        rlen = HTTP_REASON_MAX;
    memcpy(p->reason, s + 13, rlen);
    p->reason[rlen] = '\0';
    p->hdr_len = 0;                             // the status line isn't kept with the headers
    p->chunked = p->has_length = false;
    p->length = 0;
    p->state = HTTP_P_HEADERS;
}

static void headers_complete(http_resp_t *p) {
    if (p->status < 200) {
        if (p->status == 101) {
            set_error(p, "unexpected 101 Switching Protocols");
            return;
        }
        p->state = HTTP_P_STATUS;               // 100 Continue etc.: the real response follows
        p->hdr_len = 0;
        return;
    }
    if (p->chunked && p->has_length) {
        set_error(p, "response has both Content-Length and chunked encoding");
        return;
    }
    if (p->chunked && p->minor == 0) {
        set_error(p, "chunked encoding in an HTTP/1.0 response");
        return;
    }
    if (p->head || p->status == 204 || p->status == 304) {
        p->state = HTTP_P_DONE;
    } else if (p->chunked) {
        p->state = HTTP_P_CHUNK_SIZE;
    } else if (p->has_length) {
        p->remaining = p->length;
        p->state = p->length ? HTTP_P_BODY : HTTP_P_DONE;
    } else {
        p->state = HTTP_P_BODY_TO_CLOSE;
    }
}

// One complete header line, at p->hdr + p->hdr_len. Stores it as "name:value\n".
static void header_line(http_resp_t *p, size_t n) {
    char *s = p->hdr + p->hdr_len;
    if (n == 0) {
        headers_complete(p);
        return;
    }
    if (s[0] == ' ' || s[0] == '\t') {
        set_error(p, "folded header lines aren't supported");
        return;
    }
    char *colon = memchr(s, ':', n);
    if (!colon || colon == s) {
        set_error(p, "bad header line");
        return;
    }
    size_t name_len = (size_t)(colon - s);
    for (size_t i = 0; i < name_len; i++) {
        if (!is_tchar(s[i])) {
            set_error(p, "bad header name");
            return;
        }
        s[i] = lower(s[i]);
    }
    const char *v = colon + 1, *v_end = s + n;
    while (v < v_end && (*v == ' ' || *v == '\t'))
        v++;
    while (v_end > v && (v_end[-1] == ' ' || v_end[-1] == '\t'))
        v_end--;
    size_t v_len = (size_t)(v_end - v);

    if (ieq(s, name_len, "content-length")) {
        if (v_len == 0 || v_len > 15) {
            set_error(p, "bad Content-Length");
            return;
        }
        uint64_t len = 0;
        for (size_t i = 0; i < v_len; i++) {
            if (!is_digit(v[i])) {
                set_error(p, "bad Content-Length");
                return;
            }
            len = len * 10 + (uint64_t)(v[i] - '0');
        }
        if (p->has_length && len != p->length) {
            set_error(p, "conflicting Content-Length headers");
            return;
        }
        p->has_length = true;
        p->length = len;
    } else if (ieq(s, name_len, "transfer-encoding")) {
        if (!ieq(v, v_len, "chunked") || p->chunked) {
            set_error(p, "unsupported Transfer-Encoding");
            return;
        }
        p->chunked = true;
    }

    memmove(colon + 1, v, v_len);               // "name:value\n"
    colon[1 + v_len] = '\n';
    p->hdr_len += name_len + 1 + v_len + 1;
}

// One byte of the status line or a header line.
static void line_byte(http_resp_t *p, char c) {
    if (c == '\n') {
        size_t n = p->line_len;
        p->line_len = 0;
        p->line_cr = false;
        if (p->state == HTTP_P_STATUS)
            status_line(p, p->hdr + p->hdr_len, n);
        else
            header_line(p, n);
        return;
    }
    if (p->line_cr) {
        set_error(p, "CR without LF in the response headers");
        return;
    }
    if (c == '\r') {
        p->line_cr = true;
        return;
    }
    if (((unsigned char)c < 0x20 && c != '\t') || c == 0x7f) {
        set_error(p, "control character in the response headers");
        return;
    }
    // Room for the line and the stored entry's '\n'.
    if (p->hdr_len + p->line_len + 2 > HTTP_HEADERS_MAX) {
        set_error(p, "response headers too long");
        return;
    }
    p->hdr[p->hdr_len + p->line_len++] = c;
}

static void chunk_size_byte(http_resp_t *p, char c) {
    if (++p->chunk_line > CHUNK_LINE_MAX) {
        set_error(p, "chunk-size line too long");
        return;
    }
    if (c == '\n') {
        if (!p->chunk_digits) {
            set_error(p, "bad chunk size");
            return;
        }
        p->remaining = p->chunk_size;
        p->state = p->chunk_size ? HTTP_P_CHUNK_DATA : HTTP_P_TRAILERS;
        p->chunk_line = 0;
        p->chunk_size = 0;
        p->chunk_digits = p->chunk_ext = p->chunk_ws = p->line_cr = false;
        p->trailer_bytes = 0;
        return;
    }
    if (p->line_cr) {
        set_error(p, "CR without LF in a chunk-size line");
        return;
    }
    if (c == '\r') {
        p->line_cr = true;
        return;
    }
    if (((unsigned char)c < 0x20 && c != '\t') || c == 0x7f) {
        set_error(p, "control character in a chunk-size line");
        return;
    }
    if (p->chunk_ext)
        return;                                 // chunk extensions are ignored
    int v = hex_value(c);
    if (v >= 0 && !p->chunk_ws) {
        if (p->chunk_size >= CHUNK_SIZE_LIMIT / 16) {
            set_error(p, "chunk too large");
            return;
        }
        p->chunk_size = p->chunk_size * 16 + (uint32_t)v;
        p->chunk_digits = true;
    } else if ((c == ' ' || c == '\t') && p->chunk_digits) {
        p->chunk_ws = true;
    } else if (c == ';' && p->chunk_digits) {
        p->chunk_ext = true;
    } else {
        set_error(p, "bad chunk size");
    }
}

static void chunk_end_byte(http_resp_t *p, char c) {
    if (c == '\r' && !p->line_cr) {
        p->line_cr = true;
    } else if (c == '\n') {
        p->line_cr = false;
        p->state = HTTP_P_CHUNK_SIZE;
    } else {
        set_error(p, "missing CRLF after a chunk");
    }
}

static void trailer_byte(http_resp_t *p, char c) {
    if (++p->trailer_bytes > TRAILERS_MAX) {
        set_error(p, "trailers too long");
        return;
    }
    if (c == '\n') {
        bool blank = p->chunk_line == 0;
        p->chunk_line = 0;
        p->line_cr = false;
        if (blank)
            p->state = HTTP_P_DONE;
        return;
    }
    if (p->line_cr) {
        set_error(p, "CR without LF in the trailers");
        return;
    }
    if (c == '\r') {
        p->line_cr = true;
        return;
    }
    if (((unsigned char)c < 0x20 && c != '\t') || c == 0x7f) {
        set_error(p, "control character in the trailers");
        return;
    }
    p->chunk_line++;
}

size_t http_resp_feed(http_resp_t *p, const uint8_t *in, size_t len, size_t max_body,
                      const uint8_t **body, size_t *body_len) {
    *body = NULL;
    *body_len = 0;
    size_t i = 0;
    while (i < len) {
        switch (p->state) {
        case HTTP_P_STATUS:
        case HTTP_P_HEADERS:
            line_byte(p, (char)in[i++]);
            break;
        case HTTP_P_CHUNK_SIZE:
            chunk_size_byte(p, (char)in[i++]);
            break;
        case HTTP_P_CHUNK_END:
            chunk_end_byte(p, (char)in[i++]);
            break;
        case HTTP_P_TRAILERS:
            trailer_byte(p, (char)in[i++]);
            break;
        case HTTP_P_BODY:
        case HTTP_P_BODY_TO_CLOSE:
        case HTTP_P_CHUNK_DATA: {
            size_t n = len - i;
            if (n > max_body)
                n = max_body;
            if (p->state != HTTP_P_BODY_TO_CLOSE && n > p->remaining)
                n = (size_t)p->remaining;
            if (n == 0)
                return i;                       // the caller has no room for body bytes now
            *body = in + i;
            *body_len = n;
            i += n;
            p->body_received += n;
            if (p->state != HTTP_P_BODY_TO_CLOSE) {
                p->remaining -= n;
                if (p->remaining == 0)
                    p->state = p->state == HTTP_P_BODY ? HTTP_P_DONE : HTTP_P_CHUNK_END;
            }
            return i;
        }
        case HTTP_P_DONE:
        case HTTP_P_ERROR:
            return i;
        }
    }
    return i;
}

bool http_resp_eof(http_resp_t *p) {
    if (p->state == HTTP_P_BODY_TO_CLOSE)
        p->state = HTTP_P_DONE;
    if (p->state == HTTP_P_DONE)
        return true;
    if (p->state != HTTP_P_ERROR)
        set_error(p, "connection closed before the response ended");
    return false;
}

// The value of header name in the store, or NULL.
static const char *find_header(const http_resp_t *p, const char *name, size_t *v_len) {
    size_t name_len = strlen(name);
    const char *s = p->hdr, *end = p->hdr + p->hdr_len;
    while (s < end) {
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        const char *colon = memchr(s, ':', (size_t)(nl - s));
        if (colon && ieq(s, (size_t)(colon - s), name) && (size_t)(colon - s) == name_len) {
            *v_len = (size_t)(nl - colon - 1);
            return colon + 1;
        }
        s = nl + 1;
    }
    return NULL;
}

const char *http_resp_header(const http_resp_t *p, const char *name, char *buf, size_t size) {
    size_t v_len;
    const char *v = find_header(p, name, &v_len);
    if (!v || v_len + 1 > size)
        return NULL;
    memcpy(buf, v, v_len);
    buf[v_len] = '\0';
    return buf;
}

bool http_resp_has_header(const http_resp_t *p, const char *name) {
    size_t v_len;
    return find_header(p, name, &v_len) != NULL;
}
