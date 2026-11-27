#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# The manual pages: check man/*.txt and build man/pages.bundle from them, the one file
# `man install` downloads from GitHub (a line "@@ <topic>" starts each page).
#
#   tools/man-bundle.sh          check the pages, rebuild the bundle
#   tools/man-bundle.sh --check  check the pages and that the bundle is current (a host test)
#
# A page is man/<topic>.txt: the topic is a command name (letters, digits, '-', '_'); it starts
# with "NAME" and a line "    <topic> - <one-line description>"; lines are at most 80 columns,
# no tabs, none starting with "@@", and the file ends with a newline.

set -euo pipefail
cd "$(dirname "$0")/.."

check=false
[[ ${1:-} == --check ]] && check=true

status=0
fail() { echo "$1" >&2; status=1; }

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

n=0
{
    echo "# SOS manual pages, built by tools/man-bundle.sh from man/*.txt; '@@ <topic>' starts a page."
    for f in man/*.txt; do
        [[ -e $f ]] || { fail "no pages in man/"; break; }
        t=$(basename "$f" .txt)
        [[ $t =~ ^[a-z0-9][a-z0-9_-]{0,31}$ ]] || fail "$f: the topic must be a command name"
        [[ $(sed -n 1p "$f") == NAME ]] || fail "$f: line 1 must be NAME"
        [[ $(sed -n 2p "$f") == "    $t - "?* ]] || fail "$f: line 2 must be '    $t - <description>'"
        [[ $(tail -c 1 "$f" | od -An -c | tr -d ' ') == '\n' ]] || fail "$f: must end with a newline"
        LC_ALL=C.UTF-8 awk -v f="$f" '
            length($0) > 80 { print f ": line " NR " is longer than 80 columns" > "/dev/stderr"; bad = 1 }
            /\t/            { print f ": line " NR " has a tab" > "/dev/stderr"; bad = 1 }
            /^@@/           { print f ": line " NR " starts with @@" > "/dev/stderr"; bad = 1 }
            END { exit bad }' "$f" || status=1
        echo "@@ $t"
        cat "$f"
        n=$((n + 1))
    done
} > "$tmp"

if $check; then
    if ! cmp -s "$tmp" man/pages.bundle; then
        fail "man/pages.bundle is out of date: run tools/man-bundle.sh and commit it"
    fi
    [[ $status == 0 ]] && echo "man: $n pages, bundle current"
else
    [[ $status == 0 ]] || { echo "not rebuilt: fix the pages first" >&2; exit 1; }
    mv "$tmp" man/pages.bundle
    trap - EXIT
    echo "man/pages.bundle: $n pages, $(wc -c < man/pages.bundle) bytes"
fi
exit $status
