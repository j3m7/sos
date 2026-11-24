// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Wi-Fi station connection, driven from the main loop without blocking: connects with the
// network in the live configuration, retries after failures and reconnects if the link drops.

#ifndef SOS_WIFI_H
#define SOS_WIFI_H

#include <stdbool.h>

// Enable station mode and start connecting if a network is configured. After cyw43_arch_init().
void wifi_start(void);

// Advance the connection: call from the main loop. Logs state changes.
void wifi_poll(void);

// Drop the current connection and connect again with the (possibly changed) configuration.
void wifi_reconnect(void);

// True while associated and holding an IP address.
bool wifi_up(void);

// Short description of the state, e.g. "connected", "connecting", "not configured".
const char *wifi_state(void);

#endif
