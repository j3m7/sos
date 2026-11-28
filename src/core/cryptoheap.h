// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

// Heap accounting for wolfSSL, wolfCrypt and wolfSSH: every allocation they make goes through
// counting allocators, under the heap tag HEAP_NET (core/heaptag.h). Not only TLS's: the core's
// own crypto goes through them too (sealing, the boot known-answer tests, sos.hash, making the
// host key), so they're installed in every build (docs/design/build-features.md §4.2).

#ifndef SOS_CRYPTOHEAP_H
#define SOS_CRYPTOHEAP_H

#include <stdint.h>

// Install the counting allocators. Call once in main(), before wolfCrypt_Init() and any other
// wolfSSL/wolfSSH call.
void cryptoheap_init(void);

// Bytes the libraries hold now (malloc_usable_size of each block).
int64_t cryptoheap_in_use(void);

// One watcher, told the new total after each allocation or reallocation: TLS keeps each open
// connection's peak with it (tls/tls.c). NULL: none.
typedef void (*cryptoheap_watch_fn)(int64_t in_use);
void cryptoheap_watch(cryptoheap_watch_fn fn);

#endif
