// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "tls/certbundle.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "core/config.h"         // config_base64_decode()
#include "core/heaptag.h"

#define PEM_BEGIN "-----BEGIN "
#define PEM_CERT_BEGIN "-----BEGIN CERTIFICATE-----"
#define PEM_CERT_END "-----END CERTIFICATE-----"

static const uint8_t *find(const uint8_t *hay, size_t len, const char *needle) {
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= len; i++)
        if (hay[i] == (uint8_t)needle[0] && !memcmp(hay + i, needle, n))
            return hay + i;
    return NULL;
}

// Length of the DER SEQUENCE at p (tag, length, contents), or 0 if it isn't one or doesn't fit.
static size_t der_sequence_len(const uint8_t *p, size_t avail) {
    if (avail < 2 || p[0] != 0x30)
        return 0;
    size_t hdr = 2, len = p[1];
    if (len & 0x80) {
        size_t n = len & 0x7f;
        if (n == 0 || n > 3 || avail < 2 + n)
            return 0;
        len = 0;
        for (size_t i = 0; i < n; i++)
            len = len << 8 | p[2 + i];
        hdr += n;
    }
    return hdr + len <= avail ? hdr + len : 0;
}

static size_t parse_der(const uint8_t *file, size_t len, uint8_t *out, size_t out_cap,
                        certbundle_cert_t *certs, size_t max, const char **why) {
    size_t off = 0, count = 0;
    while (off < len) {
        size_t n = der_sequence_len(file + off, len - off);
        if (!n) {
            *why = "not a certificate (bad DER)";
            return 0;
        }
        if (count == max) {
            *why = "too many certificates";
            return 0;
        }
        if (off + n > out_cap) {
            *why = "output buffer too small";
            return 0;
        }
        memcpy(out + off, file + off, n);
        certs[count].off = off;
        certs[count++].len = n;
        off += n;
    }
    return count;
}

static size_t parse_pem(const uint8_t *file, size_t len, uint8_t *out, size_t out_cap,
                        certbundle_cert_t *certs, size_t max, const char **why) {
    size_t count = 0, out_len = 0;
    const uint8_t *p = file, *end = file + len;
    char *b64 = heap_alloc(HEAP_NET, len);                    // a block's base64 without whitespace
    if (!b64) {
        *why = "out of memory";
        return 0;
    }
    for (;;) {
        const uint8_t *begin = find(p, (size_t)(end - p), PEM_BEGIN);
        if (!begin)
            break;
        size_t avail = (size_t)(end - begin);
        if (avail < strlen(PEM_CERT_BEGIN) ||
            memcmp(begin, PEM_CERT_BEGIN, strlen(PEM_CERT_BEGIN))) {
            *why = "the file holds a PEM block that isn't a certificate";
            count = 0;
            break;
        }
        const uint8_t *body = begin + strlen(PEM_CERT_BEGIN);
        const uint8_t *stop = find(body, (size_t)(end - body), PEM_CERT_END);
        if (!stop) {
            *why = "unterminated certificate block";
            count = 0;
            break;
        }
        if (count == max) {
            *why = "too many certificates";
            count = 0;
            break;
        }
        size_t n = 0;
        for (const uint8_t *q = body; q < stop; q++)
            if (*q != '\n' && *q != '\r' && *q != ' ' && *q != '\t')
                b64[n++] = (char)*q;
        int der = config_base64_decode(b64, n, out + out_len, out_cap - out_len);
        if (der <= 0 || der_sequence_len(out + out_len, (size_t)der) != (size_t)der) {
            *why = "a certificate block isn't valid base64 DER";
            count = 0;
            break;
        }
        certs[count].off = out_len;
        certs[count++].len = (size_t)der;
        out_len += (size_t)der;
        p = stop + strlen(PEM_CERT_END);
    }
    heap_release(HEAP_NET, b64);
    if (count == 0 && !*why)
        *why = "no certificates in the file";
    return count;
}

size_t certbundle_parse(const uint8_t *file, size_t len, uint8_t *out, size_t out_cap,
                        certbundle_cert_t *certs, size_t max, const char **why) {
    *why = NULL;
    if (len == 0) {
        *why = "the file is empty";
        return 0;
    }
    if (find(file, len, PEM_BEGIN))
        return parse_pem(file, len, out, out_cap, certs, max, why);
    return parse_der(file, len, out, out_cap, certs, max, why);
}
