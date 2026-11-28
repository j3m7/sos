// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "files/bigjob.h"

#include <stddef.h>

static const char *running;

const char *bigjob_try_begin(const char *name) {
    if (running)
        return running;
    running = name;
    return NULL;
}

void bigjob_end(void) {
    running = NULL;
}

const char *bigjob_running(void) {
    return running;
}
