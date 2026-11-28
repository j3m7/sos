// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "time/tzbundle.h"

#include <string.h>

#include "time/tzrule.h"

#define HEADER "# tzdata "

static char fold(char c) {
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c == ' ' ? '_' : c;
}

// The characters tools/tz-bundle.sh --check allows (keep in step).
static bool name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '/' || c == '_' || c == '-' || c == '+';
}

static bool rule_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '<' || c == '>' || c == '+' || c == '-' || c == ',' || c == '.' || c == '/' ||
           c == ':';
}

bool tzbundle_version(const char *line, char out[TZBUNDLE_VERSION_MAX + 1]) {
    if (strncmp(line, HEADER, sizeof(HEADER) - 1))
        return false;
    const char *v = line + sizeof(HEADER) - 1;
    size_t n = 0;
    while ((v[n] >= '0' && v[n] <= '9') || (v[n] >= 'a' && v[n] <= 'z'))
        n++;
    if (n == 0 || n > TZBUNDLE_VERSION_MAX || v[n] != ':')
        return false;
    memcpy(out, v, n);
    out[n] = '\0';
    return true;
}

bool tzbundle_split(char *line, char **name, char **rule) {
    size_t len = strlen(line);
    if (len && line[len - 1] == '\n')
        line[--len] = '\0';
    if (len && line[len - 1] == '\r')
        line[--len] = '\0';
    char *sp = strchr(line, ' ');
    if (!sp || sp == line || sp - line > TZBUNDLE_NAME_MAX || !sp[1] ||
        strlen(sp + 1) > TZBUNDLE_RULE_MAX)
        return false;
    for (char *p = line; p < sp; p++)
        if (!name_char(*p))
            return false;
    for (char *p = sp + 1; *p; p++)
        if (!rule_char(*p))
            return false;
    *sp = '\0';
    *name = line;
    *rule = sp + 1;
    return true;
}

static bool same_name(const char *a, const char *b) {
    while (*a && fold(*a) == fold(*b)) {
        a++;
        b++;
    }
    return !*a && !*b;
}

// Levenshtein distance between a and b (each at most TZBUNDLE_NAME_MAX), case aside.
static unsigned edits(const char *a, size_t la, const char *b, size_t lb) {
    unsigned row[TZBUNDLE_NAME_MAX + 1];
    for (size_t j = 0; j <= lb; j++)
        row[j] = (unsigned)j;
    for (size_t i = 1; i <= la; i++) {
        unsigned diag = row[0];
        row[0] = (unsigned)i;
        for (size_t j = 1; j <= lb; j++) {
            unsigned up = row[j];
            unsigned best = diag + (fold(a[i - 1]) != fold(b[j - 1]));
            if (up + 1 < best)
                best = up + 1;
            if (row[j - 1] + 1 < best)
                best = row[j - 1] + 1;
            row[j] = best;
            diag = up;
        }
    }
    return row[lb];
}

static bool contains(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && fold(hay[i]) == fold(needle[i]))
            i++;
        if (i == n)
            return true;
    }
    return false;
}

unsigned tzbundle_distance(const char *query, const char *name) {
    size_t lq = strlen(query), ln = strlen(name);
    if (!lq || lq > TZBUNDLE_NAME_MAX || ln > TZBUNDLE_NAME_MAX)
        return 99;
    const char *last = strrchr(name, '/');
    last = last ? last + 1 : name;
    unsigned d = edits(query, lq, name, ln);
    unsigned dl = edits(query, lq, last, strlen(last));
    if (dl < d)
        d = dl;
    if (d > 1 && lq >= 3 && contains(name, query))
        d = 1;
    return d > 99 ? 99 : d;
}

bool tzbundle_has_prefix(const char *name, const char *prefix) {
    for (; *prefix; prefix++, name++)
        if (fold(*name) != fold(*prefix))
            return false;
    return true;
}

// Keep name among the closest few, if it is close enough: about a third of the query wrong.
static void consider(tzbundle_find_t *out, unsigned dist[TZBUNDLE_NEAR], const char *query,
                     const char *name) {
    unsigned d = tzbundle_distance(query, name);
    size_t lq = strlen(query);
    if (d > (lq < 6 ? 1 : lq / 3))
        return;
    unsigned i = out->near_count;
    while (i > 0 && dist[i - 1] > d)            // equal ones keep the bundle's order
        i--;
    if (i == TZBUNDLE_NEAR)
        return;
    unsigned last = out->near_count < TZBUNDLE_NEAR ? out->near_count : TZBUNDLE_NEAR - 1;
    for (unsigned k = last; k > i; k--) {
        dist[k] = dist[k - 1];
        memcpy(out->near[k], out->near[k - 1], sizeof(out->near[k]));
    }
    dist[i] = d;
    strcpy(out->near[i], name);                 // split() checked its length
    if (out->near_count < TZBUNDLE_NEAR)
        out->near_count++;
}

bool tzbundle_next_line(tzbundle_lines_t *l, char line[TZBUNDLE_READ_MAX], bool *too_long) {
    *too_long = false;
    if (l->have_next) {
        memcpy(line, l->next, TZBUNDLE_READ_MAX);
        l->have_next = false;
    } else if (!l->read(l->ctx, line, TZBUNDLE_READ_MAX)) {
        return false;
    }
    char *nl = strchr(line, '\n');
    if (nl) {
        *nl = '\0';
        return true;
    }
    // No newline: the file's last line, or the start of one longer than the reader gives at once.
    if (!l->read(l->ctx, l->next, TZBUNDLE_READ_MAX))
        return true;
    *too_long = true;
    while (!strchr(l->next, '\n'))             // the rest of it, up to its newline
        if (!l->read(l->ctx, l->next, TZBUNDLE_READ_MAX))
            break;
    return true;
}

tzbundle_result_t tzbundle_find(tzbundle_read_fn read, void *ctx, const char *name,
                                tzbundle_find_t *out) {
    tzbundle_lines_t lines = {.read = read, .ctx = ctx};
    char line[TZBUNDLE_READ_MAX];
    unsigned dist[TZBUNDLE_NEAR] = {0};
    bool too_long;
    memset(out, 0, sizeof(*out));
    if (!tzbundle_next_line(&lines, line, &too_long) || too_long ||
        !tzbundle_version(line, out->version))
        return TZBUNDLE_NOT_A_BUNDLE;
    while (tzbundle_next_line(&lines, line, &too_long)) {
        char *n, *r;
        tzrule_t rule;
        if (too_long || !tzbundle_split(line, &n, &r) || !tzrule_parse(r, &rule, NULL)) {
            out->skipped++;                     // too long, not a zone, or a rule not taken
            continue;
        }
        out->zones++;
        if (same_name(name, n)) {
            strcpy(out->name, n);
            strcpy(out->rule, r);
            return TZBUNDLE_FOUND;
        }
        consider(out, dist, name, n);
    }
    return TZBUNDLE_NOT_FOUND;
}

bool tzbundle_names(tzbundle_read_fn read, void *ctx, void (*fn)(const char *name, void *arg),
                    void *arg) {
    tzbundle_lines_t lines = {.read = read, .ctx = ctx};
    char line[TZBUNDLE_READ_MAX], version[TZBUNDLE_VERSION_MAX + 1];
    bool too_long;
    if (!tzbundle_next_line(&lines, line, &too_long) || too_long || !tzbundle_version(line, version))
        return false;
    while (tzbundle_next_line(&lines, line, &too_long)) {
        char *n, *r;
        if (!too_long && tzbundle_split(line, &n, &r))
            fn(n, arg);
    }
    return true;
}
