// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The time zone shown on screen (#240): `time zone` and its subcommands, and the `local:` line
// of `time`. The board keeps UTC everywhere (the RTC, file times, logs); the zone only changes
// what `time` shows. Its name and POSIX rule are saved in the configuration (tz_name, tz_rule),
// taken from the bundle on the card, /tz/zones.bundle (time/tzbundle.h), so a board shows local
// time with the card out too.
//
//   time zone                    the zone, its rule, and whether the card's bundle has another
//   time zone <name>             look the name up on the card and save it (console only)
//   time zone utc                clear it (console only)
//   time zone list [<prefix>]    the card's zones (a job)
//   time zone install            download tz/zones.bundle from the repository's main branch
//                                (a job); tools/tz-install.sh copies it over SFTP instead

#ifndef SOS_TZZONE_H
#define SOS_TZZONE_H

#include <stdint.h>

#include "shell/shell.h"

#define TZZONE_DIR    "/tz"
#define TZZONE_BUNDLE TZZONE_DIR "/zones.bundle"

// "local:  2026-11-27 14:03:05 CET (UTC+01:00, Europe/Madrid)" for t (UTC seconds), when a
// zone is set; nothing when none is.
void tzzone_print_local(shell_t *sh, int64_t t);

// `time zone ...`: args is what follows "zone" (may start with spaces, may be empty).
void tzzone_command(shell_t *sh, const char *args);

#endif
