// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "files/path.h"

#include <string.h>

bool path_resolve(const char *cwd, const char *path, char *out, size_t n) {
    if (n < 2)
        return false;
    size_t len = 0;                         // out holds "" for "/" while building
    // Relative paths start from cwd, absolute ones from the root.
    for (int pass = (path[0] == '/') ? 1 : 0; pass < 2; pass++) {
        const char *p = pass == 0 ? cwd : path;
        while (*p) {
            while (*p == '/')
                p++;
            const char *start = p;
            while (*p && *p != '/')
                p++;
            size_t clen = (size_t)(p - start);
            if (clen == 0 || (clen == 1 && start[0] == '.'))
                continue;
            if (clen == 2 && start[0] == '.' && start[1] == '.') {
                while (len > 0 && out[len - 1] != '/')
                    len--;
                if (len > 0)
                    len--;                  // drop the '/' too
                continue;
            }
            if (len + 1 + clen + 1 > n)
                return false;
            out[len++] = '/';
            memcpy(out + len, start, clen);
            len += clen;
        }
    }
    if (len == 0)
        out[len++] = '/';
    out[len] = '\0';
    return true;
}

const char *path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// FatFs (ff.c create_name) splits on '/' and '\', recognises "." and ".." only exactly, strips
// trailing spaces and dots from each name, and compares names case-insensitively. Its case
// table only folds ASCII letters to ASCII, so no non-ASCII name can match "certs". It also takes
// any control character (< 0x20) as the end of the path.
// Is the component c[0..n) the name `want` (lower case) as FatFs would compare them: without
// trailing spaces and dots, ASCII letters case-insensitively?
static bool name_is(const char *c, size_t n, const char *want) {
    while (n > 0 && (c[n - 1] == ' ' || c[n - 1] == '.'))
        n--;
    if (n != strlen(want))
        return false;
    for (size_t i = 0; i < n; i++) {
        char a = c[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (a != want[i])
            return false;
    }
    return true;
}

bool path_is_protected(const char *path) {
    // norm zeroed: what's read below always ends in a NUL, whatever path_resolve() leaves.
    char raw[512], norm[512] = {0};
    size_t len = strlen(path);
    if (len >= sizeof(raw))
        return true;                            // too long to check: refuse
    for (size_t i = 0; i < len; i++) {
        char c = path[i];
        if (c == ':')
            return true;                        // drive prefix ("0:/certs"); invalid in names
        // FatFs ends the path at any control character ("/certs\x01" is "/certs" to it), so
        // what follows wouldn't be what gets checked here: refuse.
        if ((unsigned char)c < 0x20 || c == 0x7f)
            return true;
        raw[i] = c == '\\' ? '/' : c;
    }
    raw[len] = '\0';
    if (!path_resolve("/", raw, norm, sizeof(norm)))
        return true;

    // The first two components. /certs and what's in it; /var/log and what's in it; and /var
    // itself (renaming or removing it would take /var/log along), but not the rest of /var.
    const char *first = norm + 1;
    size_t first_len = strcspn(first, "/");
    const char *second = first[first_len] ? first + first_len + 1 : NULL;
    size_t second_len = second ? strcspn(second, "/") : 0;
    if (name_is(first, first_len, PATH_PROTECTED_DIR))
        return true;
    if (name_is(first, first_len, PATH_LOG_PARENT))
        return !second || !second_len || name_is(second, second_len, PATH_LOG_DIR);
    return false;
}

bool path_join(const char *dir, const char *name, char *out, size_t n) {
    size_t dlen = strcmp(dir, "/") ? strlen(dir) : 0;
    size_t nlen = strlen(name);
    if (dlen + 1 + nlen + 1 > n)
        return false;
    memcpy(out, dir, dlen);
    out[dlen] = '/';
    memcpy(out + dlen + 1, name, nlen + 1);
    return true;
}
