#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# Runs shellcheck on the repository's shell scripts: every tracked file whose first line is a
# sh or bash shebang (tools/*.sh, tools/test/, tools/host/pico-vm-attach, .githooks/pre-commit).
# Settings in .shellcheckrc.
#
#   tools/lint-shell.sh            check the scripts in the working tree (`just lint`)
#   tools/lint-shell.sh <dir>      check the scripts under <dir>, a checkout of the repository
#                                  (the pre-commit hook passes one of the staged files)
#
# Exits 0 when clean, 1 with shellcheck's findings, 2 if shellcheck isn't installed.

set -euo pipefail
cd "${1:-$(dirname "$0")/..}"

if ! command -v shellcheck >/dev/null; then
    echo "lint-shell: shellcheck isn't installed (https://www.shellcheck.net/)" >&2
    exit 2
fi

scripts=()
while IFS= read -r -d '' f; do
    [[ -f $f ]] && head -n1 "$f" | grep -qE '^#!.*(/|env )(ba)?sh\b' && scripts+=("$f")
done < <(if git rev-parse --git-dir >/dev/null 2>&1; then git ls-files -z; else find . -type f -print0; fi)

[[ ${#scripts[@]} -eq 0 ]] && exit 0
shellcheck "${scripts[@]}"
