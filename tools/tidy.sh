#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# clang-tidy (checks in .clang-tidy) on SOS's own C sources: src/ and tools/host/, not the
# third-party code (Lua, wolfSSL, wolfSSH, the SD driver, the copied pico-sshd).
#
#   tools/tidy.sh [file...]      all of SOS's sources, or only the files named
#
# Two compile databases: the host tests' (build-tests/), which covers the sources the tests
# build, and the firmware's (build/, the debug preset) for the rest, read for the Cortex-M33
# with the ARM toolchain's headers. Either is configured here if it doesn't exist yet; the
# firmware's also needs one build for its generated files. Exits 1 if there are findings.

set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD

command -v clang-tidy >/dev/null || { echo "tidy: clang-tidy isn't installed" >&2; exit 2; }

[[ -f build-tests/compile_commands.json ]] ||
    cmake -S tests -B build-tests -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
if [[ ! -f build/compile_commands.json ]]; then
    cmake --preset debug >/dev/null
fi
[[ -f build/generated/sos_version.c ]] || cmake --build build >/dev/null

# Which database each source is read from: the host's when the tests build it.
mapfile -t lists < <(python3 - "$root" "$@" <<'EOF'
import json, os, sys
root, wanted = sys.argv[1], {os.path.abspath(f) for f in sys.argv[2:]}
def ours(f):
    rel = os.path.relpath(f, root)
    return (rel.startswith(("src/", "tools/host/")) and rel.endswith(".c")
            and "pico-sshd" not in rel and rel != "src/ssh/sftp_wolf.c")
host = {e["file"] for e in json.load(open(f"{root}/build-tests/compile_commands.json")) if ours(e["file"])}
fw = {e["file"] for e in json.load(open(f"{root}/build/compile_commands.json")) if ours(e["file"])} - host
if wanted:
    host, fw = host & wanted, fw & wanted
print(" ".join(sorted(host)))
print(" ".join(sorted(fw)))
EOF
)
read -ra host <<<"${lists[0]:-}"
read -ra fw <<<"${lists[1]:-}"
if [[ $# -gt 0 && ${#host[@]} -eq 0 && ${#fw[@]} -eq 0 ]]; then
    echo "tidy: none of those files is one of SOS's sources in a compile database" >&2
    exit 2
fi

out=$(mktemp)
trap 'rm -f "$out"' EXIT
filter="^$root/(src|tools/host)/"
jobs=$(nproc 2>/dev/null || echo 4)
common=(--quiet "--header-filter=$filter" --extra-arg=-Wno-unknown-warning-option)

if [[ ${#host[@]} -gt 0 ]]; then
    printf '%s\0' "${host[@]}" |
        xargs -0 -P "$jobs" -n 4 clang-tidy -p build-tests "${common[@]}" >>"$out" 2>/dev/null || true
fi
if [[ ${#fw[@]} -gt 0 ]]; then
    # The firmware is compiled by arm-none-eabi-gcc: give clang the target and that
    # toolchain's header directories.
    gcc_inc=$(arm-none-eabi-gcc -print-file-name=include)
    sys_inc=$(arm-none-eabi-gcc -print-sysroot 2>/dev/null)/include
    if [[ ! -d $sys_inc ]]; then
        libgcc=$(arm-none-eabi-gcc -print-libgcc-file-name)
        sys_inc=$(dirname "$libgcc")/../../../../arm-none-eabi/include
    fi
    printf '%s\0' "${fw[@]}" |
        xargs -0 -P "$jobs" -n 4 clang-tidy -p build "${common[@]}" \
            --extra-arg=--target=arm-none-eabi --extra-arg=-mcpu=cortex-m33 \
            "--extra-arg=-isystem$gcc_inc" "--extra-arg=-isystem$sys_inc" >>"$out" 2>/dev/null || true
fi

# One report per finding (a header's finding comes once per file that includes it), in SOS's
# files only: the analyzer reports a path that ends in a library header there.
findings=$(grep -E "^$root/.*: (warning|error): " "$out" | sort -u || true)
echo "tidy: $((${#host[@]} + ${#fw[@]})) files (${#host[@]} host, ${#fw[@]} firmware)"
if [[ -n $findings ]]; then
    sed "s|^$root/||" <<<"$findings"
    echo "tidy: $(wc -l <<<"$findings") findings"
    exit 1
fi
echo "tidy: no findings"
