// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Built-in TLS trust anchors (root CA certificates, DER), compiled into flash. Kept small: only
// the roots for servers SOS talks to (see issue #2 for loading more from the SD card).

#ifndef SOS_TLS_ROOTS_H
#define SOS_TLS_ROOTS_H

typedef struct {
    const char *name;
    const unsigned char *der;
    unsigned len;
} tls_root_t;

extern const tls_root_t tls_roots[];
extern const unsigned tls_root_count;

#endif
