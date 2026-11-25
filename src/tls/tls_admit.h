// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_TLS_ADMIT_H
#define SOS_TLS_ADMIT_H

#include <stddef.h>

// Whether another TLS connection may open (tls_open()). Pure C, host tested.
//
// Two limits. Requests people start (`http`, `sos.http`) may hold TLS_MAX_USER_CONNS of the
// TLS_MAX_CONNS connections, so one is always left for the system (NTS key exchange): however
// many sessions run `http` at once, time sync never fails for want of a connection. And a
// connection needs TLS_HEAP_NEEDED bytes of free heap to start, roughly what one takes at the
// peak of its handshake: rather than let wolfSSL run out halfway (and SSH sessions, Lua or the
// editor fail to allocate meanwhile), it's refused with a clear reason.

#define TLS_MAX_CONNS       3
#define TLS_MAX_USER_CONNS  2
// Measured on the board: a handshake's wolfSSL heap peaks at 22-24 KB (NTS to Cloudflare, HTTPS
// to httpbin), plus the 11.7 KB receive buffer and the caller's buffers: about 40 KB. The rest is
// headroom for everything else allocating meanwhile.
#define TLS_HEAP_NEEDED     (64 * 1024)

typedef enum {
    TLS_SYSTEM,         // the firmware's own (NTS)
    TLS_USER,           // started by someone (http, sos.http)
} tls_use_t;

// NULL if a connection for `use` may open, with `open` connections open, of which `open_user` are
// TLS_USER and `opening` haven't finished their handshake, and `heap_free` bytes of heap free;
// otherwise why not. Connections still opening haven't reached their peak, so the free heap must
// cover TLS_HEAP_NEEDED for each of them as well as for the new one: two requests started in the
// same second don't both count on the same free memory.
const char *tls_admit(unsigned open, unsigned open_user, unsigned opening, tls_use_t use,
                      size_t heap_free);

#endif
