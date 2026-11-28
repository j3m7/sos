// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_HEAPTAG_H
#define SOS_HEAPTAG_H

#include <stddef.h>

// Heap use by feature, for `mem`: malloc and friends with a tag naming what the memory is for.
// Each tag keeps the bytes it holds now and the most it has held. A block is counted at the size
// malloc really gave it (malloc_usable_size), so freeing needs no size and blocks carry no
// header; malloc's own bookkeeping per block isn't counted. Pure C, safe from both cores.
typedef enum {
    HEAP_NET,       // SSH and TLS: all of wolfSSL/wolfSSH, session buffers, certificates, SFTP files.
                    // wolfCrypt's hooks don't say who asks, so its other users count here too,
                    // network or not: card encryption, sealed records, passwords, the RNG
    HEAP_LUA,       // the script's state, its source and core 1's stack
    HEAP_EDIT,      // the editor's text
    HEAP_FILES,     // ls, cat, cp, format, sd check, sha256sum
    HEAP_HTTP,      // http requests
    HEAP_TIME,      // NTS time sync
    HEAP_PING,      // ping runs
    HEAP_WIFI,      // network names a scan heard, while an SSID question wants them for Tab
    HEAP_HEALTH,    // `health full`: the RAM test's blocks and the image hash's context
    HEAP_TAGS
} heap_tag_t;

// Like malloc, calloc(1, n), realloc and free. A block must be resized and released under the
// tag it was allocated with. heap_realloc() to 0 bytes releases and returns NULL.
void *heap_alloc(heap_tag_t tag, size_t n);
void *heap_zalloc(heap_tag_t tag, size_t n);
void *heap_realloc(heap_tag_t tag, void *p, size_t n);
void heap_release(heap_tag_t tag, void *p);
char *heap_strdup(heap_tag_t tag, const char *s);

const char *heap_tag_name(heap_tag_t tag);
size_t heap_tag_used(heap_tag_t tag);       // bytes held now
size_t heap_tag_peak(heap_tag_t tag);       // the most held at once since boot

#endif
