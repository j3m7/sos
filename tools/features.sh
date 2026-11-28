#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# Builds the firmware once per feature combination worth checking (docs/design/build-features.md
# §8), each in its own directory under build-features/, and prints each image's size. Every one
# must build and link without a warning: a call left outside its #if fails here, on the plain
# Pico 2 too, even with no such board at hand.
#
#   tools/features.sh              all the combinations (`just features`)
#   tools/features.sh <name>...    only those (names as listed below)
#
# Each configure starts from the debug preset and clears the cached SOS_FEATURE_* choices, so a
# combination gets what its flags and the defaults say, not what an earlier run left. Extra
# CMake arguments for every build (dependency paths without a local.cmake, say) come from
# $SOS_CMAKE_ARGS. Exits 1 if any combination fails or warns.

set -euo pipefail
cd "$(dirname "$0")/.."

# name : CMake flags (features left unset follow their defaults: off when a need is off)
combos=(
    "full:"
    "offline:-DSOS_FEATURE_NET=OFF"
    "pico2:-DPICO_BOARD=pico2"
    "no-ssh:-DSOS_FEATURE_SSH=OFF"
    "no-lua:-DSOS_FEATURE_LUA=OFF"
    "no-http:-DSOS_FEATURE_HTTP=OFF"
    "no-nts:-DSOS_FEATURE_NTS=OFF"
    "no-tls:-DSOS_FEATURE_TLS=OFF"
    "net-only:-DSOS_FEATURE_TLS=OFF -DSOS_FEATURE_SSH=OFF"
    "minimal:-DSOS_FEATURE_NET=OFF -DSOS_FEATURE_LUA=OFF"
    "full-test:-DSOS_TEST_COMMANDS=ON"
    "offline-test:-DSOS_FEATURE_NET=OFF -DSOS_TEST_COMMANDS=ON"
)

want=("$@")
failed=0
printf '%-14s %10s %8s  %s\n' combination text bss features
for entry in "${combos[@]}"; do
    name=${entry%%:*}
    flags=${entry#*:}
    if ((${#want[@]})) && [[ " ${want[*]} " != *" $name "* ]]; then
        continue
    fi
    dir=build-features/$name
    log=$dir.log
    mkdir -p build-features
    # shellcheck disable=SC2086  # flags and SOS_CMAKE_ARGS are word lists
    if ! cmake --preset debug -B "$dir" -U 'SOS_FEATURE_*' -U PICO_BOARD ${SOS_CMAKE_ARGS:-} \
            $flags >"$log" 2>&1; then
        printf '%-14s configure failed: %s\n' "$name" "$log"
        failed=1
        continue
    fi
    features=$(sed -n 's/^-- SOS features: //p' "$log" | tail -1)
    if ! cmake --build "$dir" --target sos >>"$log" 2>&1; then
        printf '%-14s build failed: %s\n' "$name" "$log"
        failed=1
        continue
    fi
    warnings=$(grep -c 'warning:' "$log" || true)
    read -r text _ bss _ < <(arm-none-eabi-size "$dir/sos.elf" | tail -1)
    printf '%-14s %10s %8s  %s\n' "$name" "$text" "$bss" "$features"
    if ((warnings)); then
        printf '%-14s %d warnings: %s\n' "" "$warnings" "$log"
        failed=1
    fi
done
exit "$failed"
