// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The rule every command table follows (shell.h, shell_cmd_t): one handler per entry, and the
// word handler (`runv`) for every command that takes paths. Pure C, so the host tests run it.

#include "shell/shell.h"

const shell_cmd_t *shell_tables_check(const shell_cmd_t *const *tables) {
    for (const shell_cmd_t *const *t = tables; *t; t++)
        for (const shell_cmd_t *c = *t; c->name; c++)
            if (!c->run == !c->runv || ((c->flags & SHELL_PATH) && !c->runv))
                return c;
    return NULL;
}
