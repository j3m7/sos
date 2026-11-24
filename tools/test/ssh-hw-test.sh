#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# Hardware test for the SOS SSH server: runs against a board that is flashed with SOS and on
# the network. Exits with the number of failed checks (0 = all passed).
#
# Usage: tools/test/ssh-hw-test.sh [options] HOST
#   -u USER          SSH user (default: $USER)
#   -i KEY           private key authorized on the device (default: ssh's default identities)
#   -k KNOWN_HOSTS   known_hosts file holding the device's host key (default: a temporary file,
#                    filled from ssh-keyscan; the fingerprint is printed so it can be checked)
#   -n COUNT         back-to-back logins in the login loop (default: 20)
#   -q               quick: skip the login loop and the large input burst
#
# The checks use all session slots for a few seconds; don't run it while others use the board.

# check() takes its condition single-quoted on purpose and evaluates it later.
# shellcheck disable=SC2016

set -uo pipefail

user=${USER:-}
identity=
known_hosts=
loops=20
quick=0
while getopts "u:i:k:n:qh" opt; do
    case $opt in
    u) user=$OPTARG ;;
    i) identity=$OPTARG ;;
    k) known_hosts=$OPTARG ;;
    n) loops=$OPTARG ;;
    q) quick=1 ;;
    *) sed -n '/^# Usage:/,/^# The checks/s/^# \{0,1\}//p' "$0"; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
host=${1:-}
if [[ -z $host || -z $user ]]; then
    echo "usage: $0 [-u user] [-i key] [-k known_hosts] [-n count] [-q] HOST" >&2
    exit 2
fi
for tool in ssh ssh-keygen ssh-keyscan timeout; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 2; }
done

work=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$work"' EXIT

# --- helpers --------------------------------------------------------------------------------

failures=0
pass() { printf '  PASS  %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; [[ -n ${2:-} ]] && printf '        %s\n' "$2"; failures=$((failures + 1)); }
info() { printf '  info  %s\n' "$1"; }
check() { if eval "$2"; then pass "$1"; else fail "$1" "${3:-}"; fi; }

if [[ -z $known_hosts ]]; then
    known_hosts=$work/known_hosts
    if ! timeout 15 ssh-keyscan -t ed25519 "$host" >"$known_hosts" 2>/dev/null || [[ ! -s $known_hosts ]]; then
        echo "cannot reach an SSH server on $host" >&2
        exit 2
    fi
fi

base_opts=(-o UserKnownHostsFile="$known_hosts" -o StrictHostKeyChecking=yes -o BatchMode=yes
           -o ConnectTimeout=10 -o LogLevel=ERROR)
ssh_opts=("${base_opts[@]}")
[[ -n $identity ]] && ssh_opts+=(-o IdentitiesOnly=yes -i "$identity")

# Run a shell session. Commands are sent one per line with a pause between them, so each runs
# after the previous one has answered. Output (without CRs) on stdout; ssh's exit status is kept
# in $work/rc. Usage: session [pause] [ssh options...] -- cmd1 cmd2 ...
session() {
    local pause=1 extra=()
    [[ $1 =~ ^[0-9.]+$ ]] && { pause=$1; shift; }
    while [[ $1 != -- ]]; do extra+=("$1"); shift; done
    shift
    {
        sleep 1.5
        for c in "$@"; do printf '%s\n' "$c"; sleep "$pause"; done
        sleep 1
    } | timeout 60 ssh -T "${ssh_opts[@]}" "${extra[@]}" "$user@$host" 2>&1 | tr -d '\r'
    echo "${PIPESTATUS[1]}" >"$work/rc"
}
rc() { cat "$work/rc"; }

heap_in_use() { sed -n 's/^heap: *\([0-9]*\) in use.*/\1/p' | head -1; }

# Device uptime in seconds, from the `uptime` command's "up Nd HH:MM:SS".
uptime_s() {
    sed -n 's/.*up \([0-9]*\)d \([0-9]*\):\([0-9]*\):\([0-9]*\).*/\1 \2 \3 \4/p' | head -1 |
        awk '{ print $1 * 86400 + $2 * 3600 + $3 * 60 + $4 }'
}

# --- start ----------------------------------------------------------------------------------

echo "SOS SSH hardware test: $user@$host"
fp=$(ssh-keygen -lf "$known_hosts" 2>/dev/null | awk '{print $2}' | head -1)
echo "host key: $fp"

status=$(session -- uptime ssh mem logout)
if ! grep -q '^sessions:' <<<"$status"; then
    echo "login failed or unexpected output; is the key authorized and SOS running?" >&2
    printf '%s\n' "$status" | tail -5 >&2
    exit 2
fi
max=$(sed -n 's/^sessions: *[0-9]* of \([0-9]*\).*/\1/p' <<<"$status")
in_use=$(sed -n 's/^sessions: *\([0-9]*\) of.*/\1/p' <<<"$status")
heap_start=$(heap_in_use <<<"$status")
uptime_start=$(uptime_s <<<"$status")
echo "session slots: $max"
if [[ $in_use -gt 1 ]]; then
    echo "other sessions are active ($((in_use - 1))); results may be wrong. Continue anyway in 5 s..."
    sleep 5
fi

# --- 1. login, command, logout ----------------------------------------------------------------
echo "1. login and logout"
start=$(date +%s%N)
out=$(session -- uptime logout)
check "command runs" 'grep -q "^up " <<<"$out"' "$(tail -3 <<<"$out")"
check "logout exits with status 0" '[[ $(rc) == 0 ]]' "ssh exit status $(rc)"
login_ms=$(( ($(date +%s%N) - start) / 1000000 ))

# --- 2. authentication ----------------------------------------------------------------------
echo "2. authentication"
ssh-keygen -q -t ed25519 -N '' -C ssh-hw-test -f "$work/other_key"
methods=$(timeout 20 ssh -v -T "${ssh_opts[@]}" -o PubkeyAuthentication=no "nobody-$$@$host" true 2>&1 |
          tr -d '\r' | sed -n 's/.*Authentications that can continue: //p' | head -1)
check "only publickey offered" '[[ $methods == publickey ]]' "offered: '$methods'"
out=$(timeout 20 ssh -T "${base_opts[@]}" -o IdentitiesOnly=yes -i "$work/other_key" "$user@$host" true 2>&1)
check "unknown key refused" 'grep -q "Permission denied" <<<"$out"' "$out"
out=$(timeout 20 ssh -T "${ssh_opts[@]}" "nobody-$$@$host" true 2>&1)
check "wrong user refused" 'grep -q "Permission denied" <<<"$out"' "$out"
out=$(timeout 20 ssh -T "${ssh_opts[@]}" -o PubkeyAuthentication=no -o PreferredAuthentications=password "$user@$host" true 2>&1)
check "password login refused" 'grep -q "Permission denied" <<<"$out"' "$out"

# --- 3-5. concurrent sessions, busy message, slot reuse -------------------------------------
# Timeline (seconds): all sessions log in at ~0-1 and send their marker at 1.5. At 3 one more
# connection is refused. Session 1 logs out at ~4.5; at ~6 a new login takes its slot and at
# ~8.5 lists the sessions (all slots in use again). The others log out at ~9.5.
echo "3. concurrent sessions ($max)"
for i in $(seq 1 "$max"); do
    if [[ $i == 1 ]]; then
        session 1 -- "marker_$i" "" "" logout >"$work/s$i.out" &
    else
        session 1 -- "marker_$i" "" "" "" "" "" "" "marker_$i" logout >"$work/s$i.out" &
    fi
done
sleep 3
echo "4. one connection more than the slots"
busy=$(timeout 15 ssh -T "${ssh_opts[@]}" "$user@$host" true 2>&1)
check "refused with a 'too many connections' message" 'grep -q "Too many connections" <<<"$busy"' "$busy"
sleep 2.5
echo "5. slot reuse"
reuse=$(session -- uptime ssh logout)
check "new login gets the freed slot" 'grep -q "^up " <<<"$reuse"' "$(tail -2 <<<"$reuse")"
wait
for i in $(seq 1 "$max"); do
    out=$(cat "$work/s$i.out")
    foreign=$(grep -o 'marker_[0-9]*' <<<"$out" | grep -vc "^marker_$i$")
    check "session $i saw only its own output" '[[ $foreign == 0 ]] && grep -q "marker_$i: unknown command" <<<"$out"' \
        "foreign markers: $foreign"
done
listing=$reuse
check "session list shows all slots in use" "grep -q '^sessions: *$max of $max' <<<\"\$listing\"" \
    "$(grep '^sessions:' <<<"$listing")"
check "session list marks the caller" 'grep -qE "^  \* [0-9]" <<<"$listing"'

# --- 6. large input burst -------------------------------------------------------------------
if [[ $quick == 0 ]]; then
    echo "6. input burst"
    out=$( { sleep 1.5; head -c 20000 /dev/zero | tr '\0' x; printf '\nuptime\n'; sleep 3; printf 'logout\n'; sleep 1; } |
           timeout 60 ssh -T "${ssh_opts[@]}" "$user@$host" 2>&1 | tr -d '\r')
    check "session responds after 20000 bytes of input" 'grep -qE "up [0-9]+d " <<<"$out"' "$(tail -3 <<<"$out")"
fi

# --- 7. clients that vanish -----------------------------------------------------------------
echo "7. killed clients"
(timeout -s KILL 0.3 ssh -T "${ssh_opts[@]}" "$user@$host" true >/dev/null 2>&1) 2>/dev/null
(timeout -s KILL 3 ssh -T "${ssh_opts[@]}" "$user@$host" < <(sleep 10) >/dev/null 2>&1) 2>/dev/null
sleep 3
out=$(session -- ssh logout)
check "slots released (only this session in use)" 'grep -q "^sessions: *1 of" <<<"$out"' "$(grep '^sessions:' <<<"$out")"

# --- 8. back-to-back logins -----------------------------------------------------------------
if [[ $quick == 0 ]]; then
    echo "8. back-to-back logins ($loops)"
    ok=0
    reasons=()
    for n in $(seq 1 "$loops"); do
        out=$( { sleep 0.8; printf 'logout\n'; sleep 2; } | timeout 15 ssh -T "${ssh_opts[@]}" "$user@$host" 2>&1)
        rcode=$?
        if grep -q Connected <<<"$out" && [[ $rcode == 0 ]]; then
            ok=$((ok + 1))
        else
            reasons+=("#$n exit $rcode: $(grep -v '^ *$' <<<"$out" | tail -1 | tr -d '\r' | cut -c1-90)")
        fi
    done
    check "all logins succeeded" '[[ $ok == "$loops" ]]' \
        "$ok of $loops; failures: $(printf '%s; ' "${reasons[@]}")"
fi

# --- 9. resources ---------------------------------------------------------------------------
echo "9. resources"
sleep 2
out=$(session -- uptime mem ssh logout)
heap_end=$(heap_in_use <<<"$out")
uptime_end=$(uptime_s <<<"$out")
check "heap in use unchanged after the tests" '[[ -n $heap_end && $heap_end == "$heap_start" ]]' \
    "start $heap_start, end ${heap_end:-?} bytes"
info "$(grep '^stack:' <<<"$out")"
info "$(grep '^logins:' <<<"$out")"
info "login + one command + logout took ${login_ms} ms"

echo
if [[ -n ${uptime_start:-} && -n ${uptime_end:-} && $uptime_end -lt $uptime_start ]]; then
    echo "WARNING: the device rebooted during the test (uptime ${uptime_start}s -> ${uptime_end}s);"
    echo "         results are unreliable. Was it flashed or reset by someone else?"
fi
if [[ $failures == 0 ]]; then echo "All checks passed."; else echo "$failures check(s) failed."; fi
exit "$failures"
