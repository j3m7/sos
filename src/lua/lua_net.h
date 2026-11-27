// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Core 0's side of sos.wifi.scan, sos.net.resolve and sos.ping: requests that take a while
// (a scan, a DNS lookup, a ping a second), so they stay pending across main-loop passes as
// sos.http's do, and are served from the main loop only. One at a time (one script runs).
//
// The scan shares the Wi-Fi name collection with a shell's SSID question (Tab), and the two can
// cut each other short, harmlessly: a question that ends while a script's scan joined its
// collection stops it, and the script gets the list as it stands (possibly empty); a script's
// own scan that a question's Tab joined is stopped when the script has its answer, and the next
// Tab starts a new scan. A script never stops a collection it didn't start.

#ifndef SOS_LUA_NET_H
#define SOS_LUA_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Move the pending request of type `type` (LR_SCAN, LR_RESOLVE, LR_PING; lua_bridge.h) on, with
// its arguments in host/count. Returns false while it isn't finished; true once it is, with the
// answer (lua_info.h records) in out and its length in *len, or *err set.
bool lua_net_serve(int type, const char *host, uint32_t count, uint8_t *out, size_t cap,
                   size_t *len, const char **err);

// Drop whatever is pending (Ctrl-C, the script ended).
void lua_net_drop(void);

#endif
