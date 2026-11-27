#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# Copy the manual pages (man/*.txt) to a board's SD card over SFTP, into /man, where `man`
# reads them: tools/man-install.sh [user@]<host>. The board must trust your SSH key (`key add`).
# The other way is `man install` on the board, which downloads them from GitHub.

set -euo pipefail
cd "$(dirname "$0")/.."
host=${1:?usage: tools/man-install.sh [user@]<host>}
{
    echo "-mkdir /man"
    for f in man/*.txt; do
        echo "put $f /man/$(basename "$f")"
    done
} | sftp -b - "$host"
echo "installed $(ls man/*.txt | wc -l) pages in /man on $host"
