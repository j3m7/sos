// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_MAN_H
#define SOS_MAN_H

#include "shell/shell.h"

// `man [<topic>]`: the manual pages on the SD card (/man/<topic>.txt, from the repository's man/
// directory: `man install` downloads them from GitHub, tools/man-install.sh copies them over
// SFTP). Without a topic, lists the pages; with one, shows it in the pager (edit/edit.h).
void man_command(shell_t *sh, char *args);

// Tab after `man` (a shell_values_fn, shell.h): `install` and the topics of the pages on the
// card.
void man_tab_topics(shell_t *sh, shell_value_fn fn, void *arg);

#endif
