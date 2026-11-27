// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// src/time/tzbundle.c: /tz/zones.bundle as it may be on the SD card, which anyone with SFTP
// can write. The input is a name to look up, a NUL, then the bundle's bytes (NULs and all), read
// as FatFs's f_gets() would. Any input is read to the end or to the match without going out of
// bounds; a match is the name asked for with a rule tzrule_parse() takes, and every name offered
// instead is a string that fits the configuration.

#include <stdlib.h>
#include <string.h>

#include "fuzz.h"
#include "time/tzbundle.h"
#include "time/tzrule.h"

#define MUST(x) do { if (!(x)) abort(); } while (0)

typedef struct {
    const uint8_t *p;
    size_t left;
} text_t;

static bool read_text(void *ctx, char *buf, size_t cap) {
    text_t *t = ctx;
    if (!t->left)
        return false;
    size_t n = 0;
    while (n + 1 < cap && n < t->left) {
        n++;
        if (t->p[n - 1] == '\n')
            break;
    }
    memcpy(buf, t->p, n);
    buf[n] = '\0';
    t->p += n;
    t->left -= n;
    return true;
}

static char fold(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c == ' ' ? '_' : c;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const uint8_t *nul = memchr(data, 0, size);
    size_t qlen = nul ? (size_t)(nul - data) : size;
    if (qlen > TZBUNDLE_NAME_MAX)
        return 0;
    char query[TZBUNDLE_NAME_MAX + 1];
    memcpy(query, data, qlen);
    query[qlen] = '\0';
    text_t t = {nul ? nul + 1 : data + size, nul ? size - qlen - 1 : 0};
    tzbundle_find_t f;
    tzbundle_result_t r = tzbundle_find(read_text, &t, query, &f);
    MUST(memchr(f.version, 0, sizeof(f.version)));
    MUST(f.near_count <= TZBUNDLE_NEAR);
    for (unsigned i = 0; i < f.near_count; i++)
        MUST(memchr(f.near[i], 0, sizeof(f.near[i])) && f.near[i][0]);
    if (r == TZBUNDLE_FOUND) {
        MUST(strlen(f.name) == qlen);
        for (size_t i = 0; i < qlen; i++)
            MUST(fold(f.name[i]) == fold(query[i]));
        tzrule_t rule;
        MUST(tzrule_parse(f.rule, &rule, NULL));
    } else {
        MUST(r == TZBUNDLE_NOT_FOUND || r == TZBUNDLE_NOT_A_BUNDLE);
    }
    (void)tzbundle_distance(query, f.version);
    return 0;
}
