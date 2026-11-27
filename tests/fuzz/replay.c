// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Runs a fuzz target over files without libFuzzer: each argument is a file, or a directory
// whose files are all run. The host tests run every target over its corpus this way, so a
// crash once found (saved there) stays fixed, with the sanitizers of the ordinary test build.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fuzz.h"

static int run_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 1;
    }
    uint8_t *buf = NULL;
    size_t len = 0, cap = 0, n;
    do {
        if (len == cap) {
            cap = cap ? 2 * cap : 4096;
            uint8_t *b = realloc(buf, cap);
            if (!b) {
                free(buf);
                fclose(f);
                return 1;
            }
            buf = b;
        }
        n = fread(buf + len, 1, cap - len, f);
        len += n;
    } while (n);
    fclose(f);
    LLVMFuzzerTestOneInput(buf, len);
    free(buf);
    return 0;
}

int main(int argc, char **argv) {
    int files = 0, failed = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st)) {
            perror(argv[i]);
            return 1;
        }
        if (!S_ISDIR(st.st_mode)) {
            failed += run_file(argv[i]);
            files++;
            continue;
        }
        DIR *d = opendir(argv[i]);
        if (!d) {
            perror(argv[i]);
            return 1;
        }
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", argv[i], e->d_name);
            failed += run_file(path);
            files++;
        }
        closedir(d);
    }
    printf("%d inputs run\n", files);
    return failed ? 1 : 0;
}
