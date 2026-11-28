// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The CYW43 driver's port configuration for SOS, named by CYW43_CONFIG_FILE (CMakeLists.txt) so
// that the driver includes it in place of the SDK's cyw43_configport.h. It includes that file
// unchanged, then takes the driver's two reporting hooks (docs/design/health.md §9.9):
//
//   CYW43_FAIL_FAST_CHECK(res)  every error return, at its origin: counted by wifistat_fail()
//   CYW43_WARN(...)             every warning: counted by function and logged, never printed
//
// Both go to src/net/wifistat.c. The driver leaves both macros to the port (cyw43_config.h
// defines them only if undefined); CMakeLists.txt checks that at configure time.

#ifndef SOS_CYW43_SOS_H
#define SOS_CYW43_SOS_H

#include <cyw43_configport.h>

#define SOS_CYW43_HOOKS 1               // wifistat.c refuses to build without this file

// Returns res unchanged; counts it (timeout, I/O or other) and keeps the function it came from.
int wifistat_fail(int res, const char *func);
// Counts the warning by the function it comes from, and logs it at most once per site every 10 s.
void wifistat_warn(const char *func, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define CYW43_FAIL_FAST_CHECK(res) wifistat_fail((res), __func__)
#define CYW43_WARN(...) wifistat_warn(__func__, __LINE__, __VA_ARGS__)

#endif
