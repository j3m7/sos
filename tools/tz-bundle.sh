#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# The time zone bundle: tz/zones.bundle, one line per IANA zone, "<name> <POSIX TZ rule>", the
# file `time zone` reads on the card (issue #240). The rule is the footer zic writes at the end
# of each compiled zone: what the zone does now and from now on, with no history.
#
#   tools/tz-bundle.sh          build the bundle from the release named in tz/VERSION
#   tools/tz-bundle.sh --check  check the committed bundle: its header names tz/VERSION and
#                               every line is well formed and fits (a host test; no network)
#
# Building needs gpg, a C compiler and make (zic is built from the release's own tzcode) and
# access to data.iana.org. It downloads tzdata<v> and tzcode<v> with their detached signatures,
# checks both against tz/maintainers.asc, whose fingerprint is pinned below, builds zic,
# compiles the zones with it (-b slim) and takes each zone's footer. The output depends only on
# the release, so a rebuild gives the same bytes. The bundle is committed, so only whoever
# updates the release needs these tools.
#
# The key: IANA's tz releases are signed by the TZ Coordinator (RFC 6557), Paul Eggert. Its
# fingerprint below is the issuer of the signatures on the releases and the key published under
# it on keys.openpgp.org (with the e-mail address verified there); tz/maintainers.asc is
# that key. If a release is signed by another key, stop and find out why before pinning it.

set -euo pipefail
cd "$(dirname "$0")/.."

TZ_FPR=7E3792A9D8ACF7D633BC1588ED97E90E62AA7E34
TZ_URL=https://data.iana.org/time-zones/releases
NAME_MAX=39         # config tz_name[40]
RULE_MAX=63         # config tz_rule[64]

version=$(tr -d '[:space:]' < tz/VERSION)
[[ $version =~ ^[0-9]{4}[a-z]$ ]] || { echo "tz/VERSION: '$version' is not a release like 2026f" >&2; exit 1; }
header="# tzdata $version: each zone's rule from zic -b slim, built by tools/tz-bundle.sh from IANA's signed release"

# Check a bundle (file $1): the header, then lines "<name> <rule>" sorted by name, each name and
# rule made of the characters they use and within the config's sizes. Prints the zone count.
check_bundle() {
    LC_ALL=C awk -v header="$header" -v nmax="$NAME_MAX" -v rmax="$RULE_MAX" -v f="$1" '
        function bad(why) { print f ": line " NR ": " why > "/dev/stderr"; failed = 1 }
        NR == 1 { if ($0 != header) bad("the header does not match tz/VERSION (want: " header ")"); next }
        {
            if (NF != 2 || $0 != $1 " " $2) { bad("not \"<name> <rule>\""); next }
            if ($1 !~ /^[A-Za-z0-9_+-]+(\/[A-Za-z0-9_+-]+)*$/) bad("bad zone name " $1)
            if ($2 !~ /^[A-Za-z0-9<>+:,.\/-]+$/) bad("bad rule for " $1)
            if (length($1) > nmax) bad($1 ": name longer than " nmax)
            if (length($2) > rmax) bad($1 ": rule longer than " rmax)
            if (NR > 2 && $1 <= prev) bad($1 ": not sorted, or a repeat")
            prev = $1; n++
        }
        END { if (n == 0) bad("no zones"); if (!failed) print n; exit failed }' "$1"
}

if [[ ${1:-} == --check ]]; then
    n=$(check_bundle tz/zones.bundle) || { echo "tz/zones.bundle: run tools/tz-bundle.sh and commit it" >&2; exit 1; }
    echo "tz: $n zones from tzdata $version, bundle well formed"
    exit 0
fi

for tool in gpg curl make cc; do
    command -v "$tool" >/dev/null || { echo "tz-bundle: needs $tool" >&2; exit 1; }
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export GNUPGHOME="$work/gnupg"
mkdir -m 700 "$GNUPGHOME"

gpg --batch --quiet --import tz/maintainers.asc 2>/dev/null
gpg --batch --with-colons --fingerprint | grep -q "^fpr:::::::::$TZ_FPR:" ||
    { echo "tz/maintainers.asc is not the key $TZ_FPR" >&2; exit 1; }

mkdir "$work/tzdb"
for part in tzcode tzdata; do
    f="$part$version.tar.gz"
    curl -fsS -o "$work/$f" "$TZ_URL/$f"
    curl -fsS -o "$work/$f.asc" "$TZ_URL/$f.asc"
    # Good only with a valid signature made by the pinned key (VALIDSIG names the primary key's
    # fingerprint last). The key isn't certified in this keyring, so gpg's trust warning is noise.
    status=$(gpg --batch --status-fd 1 --verify "$work/$f.asc" "$work/$f" 2>/dev/null || true)
    grep -qE "^\[GNUPG:\] VALIDSIG .* $TZ_FPR\$" <<<"$status" ||
        { echo "tz-bundle: $f is not validly signed by $TZ_FPR" >&2; exit 1; }
    tar -xzf "$work/$f" -C "$work/tzdb"
done
[[ $(tr -d '[:space:]' < "$work/tzdb/version") == "$version" ]] ||
    { echo "tz-bundle: the release's version file doesn't say $version" >&2; exit 1; }

make -s -C "$work/tzdb" zic >/dev/null
data=(africa antarctica asia australasia europe northamerica southamerica etcetera factory backward)
(cd "$work/tzdb" && ./zic -b slim -d "$work/zones" "${data[@]}")

{
    echo "$header"
    (cd "$work/zones" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) | while read -r zone; do
        [[ $(head -c 4 "$work/zones/$zone") == TZif ]] || continue
        rule=$(tail -n 1 "$work/zones/$zone")
        # A zone whose future can't be written as a POSIX rule has an empty footer; zic gives one
        # for every zone today, and the check below stops the build if that ever changes.
        echo "$zone $rule"
    done
} > "$work/zones.bundle"

n=$(check_bundle "$work/zones.bundle") || { echo "tz-bundle: not written" >&2; exit 1; }
mv "$work/zones.bundle" tz/zones.bundle
echo "tz/zones.bundle: $n zones from tzdata $version"
