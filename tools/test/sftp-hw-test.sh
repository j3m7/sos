#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# Hardware test for SFTP on the SD card: runs against a board that is flashed with SOS, on the
# network, with a formatted card inserted. Exits with the number of failed checks (0 = all
# passed).
#
# Usage: tools/test/sftp-hw-test.sh [options] HOST
#   -u USER          SSH user (default: $USER)
#   -i KEY           private key authorized on the device (default: ssh's default identities)
#   -k KNOWN_HOSTS   known_hosts file holding the device's host key (default: a temporary file,
#                    filled from ssh-keyscan; the fingerprint is printed so it can be checked)
#
# It works in /sftp-hw-test on the card (removed at the end) and moves a few MB each way. Don't
# run it while others use the board: it unmounts the card, and the heap check needs it idle.

# check() takes its condition single-quoted on purpose and evaluates it later.
# shellcheck disable=SC2016

set -uo pipefail

user=${USER:-}
identity=
known_hosts=
while getopts "u:i:k:h" opt; do
    case $opt in
    u) user=$OPTARG ;;
    i) identity=$OPTARG ;;
    k) known_hosts=$OPTARG ;;
    *) sed -n '/^# Usage:/,/^# It works/s/^# \{0,1\}//p' "$0" | sed '$d'; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
host=${1:-}
if [[ -z $host || -z $user ]]; then
    echo "usage: $0 [-u user] [-i key] [-k known_hosts] HOST" >&2
    exit 2
fi
for tool in ssh sftp ssh-keygen ssh-keyscan timeout cmp head pgrep; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 2; }
done

work=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$work"' EXIT
cd "$work" || exit 2

# --- helpers --------------------------------------------------------------------------------

failures=0
pass() { printf '  PASS  %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; [[ -n ${2:-} ]] && printf '        %s\n' "$2"; failures=$((failures + 1)); }
check() { if eval "$2"; then pass "$1"; else fail "$1" "${3:-}"; fi; }

if [[ -z $known_hosts ]]; then
    known_hosts=$work/known_hosts
    if ! timeout 15 ssh-keyscan -t ed25519 "$host" >"$known_hosts" 2>/dev/null || [[ ! -s $known_hosts ]]; then
        echo "cannot reach an SSH server on $host" >&2
        exit 2
    fi
fi

opts=(-o UserKnownHostsFile="$known_hosts" -o StrictHostKeyChecking=yes -o BatchMode=yes
      -o ConnectTimeout=10 -o LogLevel=ERROR)
[[ -n $identity ]] && opts+=(-o IdentitiesOnly=yes -i "$identity")

# sftp batch commands on stdin; output (with errors) on stdout. A leading '-' on a command lets
# the batch go on when it fails.
sftp_batch() { timeout 300 sftp "${opts[@]}" -b - "$user@$host" 2>&1; }

# Shell commands, one per line with a pause between them; output without CRs.
shell() {
    { sleep 1.5; for c in "$@"; do printf '%s\n' "$c"; sleep 1; done; printf 'logout\n'; sleep 1; } |
        timeout 60 ssh -T "${opts[@]}" "$user@$host" 2>&1 | tr -d '\r'
}

heap_in_use() { sed -n 's/^heap: *\([0-9]*\) in use.*/\1/p' | head -1; }

# --- start ----------------------------------------------------------------------------------

echo "SOS SFTP hardware test: $user@$host"
fp=$(ssh-keygen -lf "$known_hosts" 2>/dev/null | awk '{print $2}' | head -1)
echo "host key: $fp"

status=$(shell ssh mem)
if ! grep -q '^sessions:' <<<"$status"; then
    echo "login failed or unexpected output; is the key authorized and SOS running?" >&2
    printf '%s\n' "$status" | tail -5 >&2
    exit 2
fi
in_use=$(sed -n 's/^sessions: *\([0-9]*\) of.*/\1/p' <<<"$status")
if [[ $in_use -gt 1 ]]; then
    echo "other sessions are active ($((in_use - 1))); results may be wrong. Continue anyway in 5 s..."
    sleep 5
fi

head -c 2097152 /dev/urandom >big.bin
head -c 1000000 big.bin >part.bin
printf 'hello sftp\n' >small.txt
d=sftp-hw-test

# Leftovers of an earlier run that stopped part way.
printf -- '-rm %s/big.bin\n-rm %s/small.txt\n-rm %s/r.bin\n-rm %s/renamed.txt\n-rmdir %s\n' \
    $d $d $d $d $d | sftp_batch >/dev/null
heap_start=$(shell mem | heap_in_use)

# --- 1. transfers ---------------------------------------------------------------------------
echo "1. put and get"
start=$(date +%s%N)
out=$(printf 'mkdir %s\nput small.txt %s/small.txt\nput big.bin %s/big.bin\n' $d $d $d | sftp_batch)
up_ms=$(( ($(date +%s%N) - start) / 1000000 ))
start=$(date +%s%N)
out+=$(printf 'get %s/big.bin big.down\nget %s/small.txt small.back\n' $d $d | sftp_batch)
down_ms=$(( ($(date +%s%N) - start) / 1000000 ))
check "2 MB file comes back identical" 'cmp -s big.bin big.down' "$(tail -3 <<<"$out")"
check "small file comes back identical" 'cmp -s small.txt small.back'
printf '  info  2 MB up in %d ms, down in %d ms (including login)\n' "$up_ms" "$down_ms"

out=$(printf 'put small.txt %s/big.bin\nget %s/big.bin ow.back\nput big.bin %s/big.bin\n' $d $d $d | sftp_batch)
check "overwriting with a smaller file truncates it" 'cmp -s small.txt ow.back' "$(tail -3 <<<"$out")"

# --- 2. resumed transfers -------------------------------------------------------------------
echo "2. resume"
cp part.bin resume.bin
out=$(printf 'reget %s/big.bin resume.bin\n' $d | sftp_batch)
check "reget resumes at 1,000,000 bytes" 'cmp -s big.bin resume.bin' "$(tail -3 <<<"$out")"
out=$(printf 'put part.bin %s/r.bin\nreput big.bin %s/r.bin\nget %s/r.bin r.back\n' $d $d $d | sftp_batch)
check "reput resumes at 1,000,000 bytes" 'cmp -s big.bin r.back' "$(tail -3 <<<"$out")"

# --- 3. directory operations ----------------------------------------------------------------
echo "3. rename and rmdir"
out=$(printf -- '-rename %s/small.txt %s/big.bin\n-rmdir %s\nrename %s/small.txt %s/renamed.txt\nls %s\n' \
    $d $d $d $d $d $d | sftp_batch)
check "rename onto an existing file is reported as a failure" 'grep -q "rename.*: Failure" <<<"$out"' "$out"
check "rmdir of a non-empty directory is reported as a failure" 'grep -q "rmdir.*: Failure" <<<"$out"' "$out"
check "rename" 'grep -q renamed.txt <<<"$out"' "$out"

# --- 4. files in use ------------------------------------------------------------------------
echo "4. files in use during a download"
( printf 'get %s/big.bin dl.bin\n' $d | sftp_batch >/dev/null ) 2>/dev/null &
sleep 4
client=$(pgrep -n -x sftp)
out=$(shell "sd unmount" "rm /$d/big.bin")
check "unmount refused during a download" 'grep -q "files are open" <<<"$out"' "$out"
check "rm of the file being downloaded refused" 'grep -q "in use by another command" <<<"$out"' "$out"

# Kill the client mid-transfer: the device notices once the requests it has queued are served
# (about 5 s measured), then closes the file.
[[ -n $client ]] && kill -9 "$client"
wait 2>/dev/null
released=
for i in 1 2 3 4 5 6; do
    sleep 2.5
    if shell "sd unmount" | grep -q unmounted; then
        released=$((i * 5 / 2))
        break
    fi
done
check "a killed client's files are released within 15 s" '[[ -n $released ]]'
[[ -n $released ]] && printf '  info  released after about %s s\n' "$released"

# --- 5. cleanup and resources ---------------------------------------------------------------
echo "5. cleanup"
out=$(printf 'rm %s/big.bin\nrm %s/r.bin\nrm %s/renamed.txt\nrmdir %s\n' $d $d $d $d | sftp_batch)
# shellcheck disable=SC2034  # used in check's condition
rc=$?
check "test files removed" '[[ $rc == 0 ]]' "$out"
heap_end=$(shell mem | heap_in_use)
check "heap in use unchanged" '[[ -n $heap_start && $heap_start == "$heap_end" ]]' \
    "heap in use $heap_start -> $heap_end bytes"

echo
if [[ $failures == 0 ]]; then echo "All checks passed."; else echo "$failures check(s) failed."; fi
exit "$failures"
