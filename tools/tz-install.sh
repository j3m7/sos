#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# Copy the time zone bundle (tz/zones.bundle) to a board's SD card over SFTP, into /tz, where
# `time zone` reads it: tools/tz-install.sh [user@]<host>. The board must trust your SSH key
# (`key add`). The other way is `time zone install` on the board, which downloads it from
# GitHub. Then `time zone <name>` on the console sets the zone (or takes its new rule).

set -euo pipefail
cd "$(dirname "$0")/.."
host=${1:?usage: tools/tz-install.sh [user@]<host>}
tools/tz-bundle.sh --check
# Written beside it and renamed into place, as `time zone install` does: a copy cut short
# leaves the old bundle.
sftp -b - "$host" <<SFTP
-mkdir /tz
put tz/zones.bundle /tz/zones.~new
-rm /tz/zones.bundle
rename /tz/zones.~new /tz/zones.bundle
SFTP
echo "installed tzdata $(cat tz/VERSION) in /tz/zones.bundle on $host"
