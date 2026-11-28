// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The big file jobs, one at a time: see bigjob.h. Only core 0's shells run them.

#include "files/bigjob.h"

#include <stddef.h>

static const char *running_job;     // the command holding the slot, or NULL

bool bigjob_begin(const char *name, const char **running) {
    if (running_job) {
        *running = running_job;
        return false;
    }
    running_job = name;
    return true;
}

void bigjob_end(void) {
    running_job = NULL;
}
