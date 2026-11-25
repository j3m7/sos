// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Admission of new TLS connections (see tls_admit.h). Depends only on the C library, so it builds
// on the host for unit tests.

#include "tls/tls_admit.h"

#define STR_(x) #x
#define STR(x) STR_(x)

const char *tls_admit(unsigned open, unsigned open_user, unsigned opening, tls_use_t use,
                      size_t heap_free) {
    if (open >= TLS_MAX_CONNS)
        return "too many TLS connections open";
    if (use == TLS_USER && open_user >= TLS_MAX_USER_CONNS)
        return "too many HTTPS requests at once (" STR(TLS_MAX_USER_CONNS)
               " at most; try again when one finishes)";
    if (heap_free / TLS_HEAP_NEEDED < (size_t)opening + 1)
        return "not enough free memory for a TLS connection";
    return 0;
}
