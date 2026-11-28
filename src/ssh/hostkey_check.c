// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The `hostkey` health check's reads (docs/design/health.md §9.3), moved from
// core/statecheck_board.c (docs/design/build-features.md §4.3): hostkey.c's digest of the stored
// page against the page now, never opened. core/statecheck.c judges.

#include "ssh/hostkey_check.h"

#include <stdarg.h>
#include <stdio.h>

#include "core/settings.h"
#include "core/statecheck.h"
#include "ssh/hostkey.h"
#include "ssh/sshd.h"

// --- hostkey ---------------------------------------------------------------------------------

static void hostkey_gather(statecheck_hostkey_t *h) {
    hostkey_health_t k;
    hostkey_health(&k);
    h->have_owner = settings_has_owner_secret();
    h->have_key = k.have_key;
    h->gave_up = k.gave_up;
    h->unsealed = k.unsealed;
    h->page_matches = k.page_matches;
    h->erased = k.erased;
    const char *fp = sshd_host_fingerprint();
    h->fingerprint = fp && fp[0] ? fp : NULL;
}

health_step_t hostkey_check_step(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel) {
        statecheck_hostkey_t h;
        hostkey_gather(&h);
        statecheck_judge_hostkey(&h, out);
    }
    return HEALTH_DONE;
}

#ifdef SOS_TEST_COMMANDS
#define DUMP_LINE 256

__attribute__((format(printf, 3, 4)))
static void dump(health_print_fn print, void *arg, const char *fmt, ...) {
    char line[DUMP_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    print(arg, line);
}

void hostkey_check_dump(health_print_fn print, void *arg) {
    statecheck_hostkey_t h;
    hostkey_gather(&h);
    dump(print, arg, "hostkey: owner %d, key %d, gave up %d, unsealed %d, page as loaded %d, %s",
         h.have_owner, h.have_key, h.gave_up, h.unsealed, h.page_matches,
         h.fingerprint ? h.fingerprint : "(sshd not started)");
}
#endif
