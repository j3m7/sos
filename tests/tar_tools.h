// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// For tests of archives SOS writes (tests/test_tar_write.c, tests/test_tar.c): hand an archive
// to the host's GNU tar or bsdtar, list it (`-tvf`) and extract it (`-xf`) into a temporary
// directory, and compare with what it should hold. A tool the host lacks is skipped with a note,
// as the other host tests skip missing tools.

#ifndef SOS_TEST_TAR_TOOLS_H
#define SOS_TEST_TAR_TOOLS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    const char *name;           // as stored, a directory's without its '/'
    bool dir;
    const uint8_t *data;        // a file's contents
    size_t len;
    int64_t mtime;              // checked on the extracted file when not 0
} tt_entry_t;

// Is `tool` ("tar", which must be GNU tar, or "bsdtar") on this host? Said once if not.
static bool tt_have(const char *tool) {
    static int have_gnu = -1, have_bsd = -1;
    bool gnu = !strcmp(tool, "tar");
    int *have = gnu ? &have_gnu : &have_bsd;
    if (*have < 0) {
        *have = system(gnu ? "tar --version 2>/dev/null | grep -q 'GNU tar'"
                           : "bsdtar --version >/dev/null 2>&1") == 0;
        if (!*have)
            printf("note: no %s on this host: its checks are skipped\n",
                   gnu ? "GNU tar" : "bsdtar");
    }
    return *have;
}

// How many files and directories extracting the entries e makes: the entries and the directories
// their names imply.
static size_t tt_paths(const tt_entry_t *e, size_t n) {
    size_t cap = 64, count = 0;
    char **seen = malloc(cap * sizeof(*seen));
    for (size_t i = 0; i < n; i++) {
        for (const char *c = e[i].name;; c++) {
            if (*c && *c != '/')
                continue;
            size_t len = (size_t)(c - e[i].name);
            bool dup = false;
            for (size_t k = 0; k < count && !dup; k++)
                dup = strlen(seen[k]) == len && !strncmp(seen[k], e[i].name, len);
            if (!dup) {
                if (count == cap)
                    seen = realloc(seen, (cap *= 2) * sizeof(*seen));
                seen[count] = malloc(len + 1);
                memcpy(seen[count], e[i].name, len);
                seen[count++][len] = '\0';
            }
            if (!*c)
                break;
        }
    }
    for (size_t k = 0; k < count; k++)
        free(seen[k]);
    free(seen);
    return count;
}

static bool tt_fail(const char *tool, const char *what, const char *name) {
    fprintf(stderr, "  %s: %s%s%s\n", tool, what, name ? ": " : "", name ? name : "");
    return false;
}

// Run `cmd` and read its output into buf (NUL-terminated, cut at n - 1). True if it exited 0.
static bool tt_run(const char *cmd, char *buf, size_t n) {
    FILE *f = popen(cmd, "r");
    if (!f)
        return false;
    size_t len = fread(buf, 1, n - 1, f);
    buf[len] = '\0';
    while (fgetc(f) != EOF) {
    }
    return pclose(f) == 0;
}

// `tool` lists the archive arc[0..len) (plain or gzipped) as exactly the n entries `e` (each
// line: the mode a Unix tar shows for 0644 files and 0755 directories, and the name, a directory's
// with '/'), and extracts it into an empty directory as those files and directories with that
// content (and dates, where given), and nothing else but the directories their names imply. True when it does, or when the host lacks `tool`.
static bool tt_check(const char *tool, const uint8_t *arc, size_t len, const tt_entry_t *e,
                     size_t n) {
    if (!tt_have(tool))
        return true;
    char dir[] = "/tmp/sos_test_tar_XXXXXX";
    if (!mkdtemp(dir))
        return tt_fail(tool, "mkdtemp", NULL);
    char path[2048], cmd[4096];
    snprintf(path, sizeof(path), "%s.arc", dir);
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(arc, 1, len, f) == len;
    if (f)
        fclose(f);
    static char out[1 << 18];
    snprintf(cmd, sizeof(cmd), "%s -tvf %s 2>&1", tool, path);
    if (ok && !tt_run(cmd, out, sizeof(out)))
        ok = tt_fail(tool, "-tvf failed", out);
    size_t lines = 0;
    for (char *line = out, *next; ok && *line; line = next) {
        next = strchr(line, '\n');
        if (!next)
            next = line + strlen(line);
        else
            *next++ = '\0';
        lines++;
        char *name = strrchr(line, ' ');
        if (!name)
            return tt_fail(tool, "odd line", line);
        name++;
        bool found = false;
        for (size_t i = 0; i < n && !found; i++) {
            size_t l = strlen(e[i].name);
            found = !strncmp(name, e[i].name, l) && !strcmp(name + l, e[i].dir ? "/" : "") &&
                    !strncmp(line, e[i].dir ? "drwxr-xr-x" : "-rw-r--r--", 10);
        }
        if (!found)
            ok = tt_fail(tool, "listed what wasn't put in, or with another mode", line);
    }
    if (ok && lines != n)
        ok = tt_fail(tool, "listed another number of entries", NULL);
    snprintf(cmd, sizeof(cmd), "cd %s && %s -xf %s 2>&1", dir, tool, path);
    if (ok && !tt_run(cmd, out, sizeof(out)))
        ok = tt_fail(tool, "-xf failed", out);
    snprintf(cmd, sizeof(cmd), "find %s -mindepth 1 | wc -l", dir);
    if (ok && (!tt_run(cmd, out, sizeof(out)) || (size_t)atol(out) != tt_paths(e, n)))
        ok = tt_fail(tool, "extracted another number of files and directories", out);
    for (size_t i = 0; ok && i < n; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, e[i].name);
        struct stat st;
        if (stat(path, &st) || S_ISDIR(st.st_mode) != e[i].dir) {
            ok = tt_fail(tool, "missing, or of another kind", e[i].name);
            break;
        }
        if (e[i].mtime && st.st_mtime != e[i].mtime)
            ok = tt_fail(tool, "another date", e[i].name);
        if (e[i].dir)
            continue;
        uint8_t *got = malloc(e[i].len + 1);
        FILE *g = fopen(path, "rb");
        size_t got_len = g ? fread(got, 1, e[i].len + 1, g) : 0;
        if (g)
            fclose(g);
        if (!g || got_len != e[i].len || (e[i].len && memcmp(got, e[i].data, e[i].len)))
            ok = tt_fail(tool, "other contents", e[i].name);
        free(got);
    }
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s.arc", dir, dir);
    if (system(cmd) != 0)
        ok = tt_fail(tool, "rm failed", dir);
    return ok;
}

#endif
