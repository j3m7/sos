// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

// The `tls` command (moved from shell/shell.c, docs/design/build-features.md §4.1).

#include "tls/tls_cmd.h"

#include <string.h>

#include "tls/certstore.h"

// `tls roots [approve | forget]`: extra TLS roots from the SD card (src/tls/certstore.c).
void tls_command(shell_t *sh, char *args) {
    if (args && !strncmp(args, "roots", 5) && (args[5] == ' ' || !args[5])) {
        char *rest = args + 5;
        while (*rest == ' ')
            rest++;
        certstore_command(sh, *rest ? rest : NULL);
    } else {
        shell_printf(sh, "usage: tls roots [approve | forget]\n");
    }
}

// The words Tab offers (shell.h).
#define END {0}
#define W(name) {name, NULL, 0}
const shell_word_t tls_words[] = {
    {"roots", (const shell_word_t[]){{"approve", NULL, SHELL_ADMIN}, {"forget", NULL, SHELL_ADMIN},
                                     END}, 0},
    END};
#undef W
#undef END
