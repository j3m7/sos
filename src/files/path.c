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
// table only folds ASCII letters to ASCII, so no non-ASCII name can match "certs".
bool path_is_protected(const char *path) {
    char raw[512], norm[512];
    size_t len = strlen(path);
    if (len >= sizeof(raw))
        return true;                            // too long to check: refuse
    for (size_t i = 0; i <= len; i++) {
        char c = path[i];
        if (c == ':')
            return true;                        // drive prefix ("0:/certs"); invalid in names
        raw[i] = c == '\\' ? '/' : c;
    }
    if (!path_resolve("/", raw, norm, sizeof(norm)))
        return true;

    // First component, without trailing spaces and dots.
    const char *c = norm + 1;
    size_t n = strcspn(c, "/");
    while (n > 0 && (c[n - 1] == ' ' || c[n - 1] == '.'))
        n--;
    const char *dir = PATH_PROTECTED_DIR;
    if (n != strlen(dir))
        return false;
    for (size_t i = 0; i < n; i++) {
        char a = c[i], b = dir[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
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
