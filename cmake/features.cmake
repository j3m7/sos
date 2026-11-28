# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# Build features (docs/design/build-features.md): one option per feature, SOS_FEATURE_<NAME>,
# ON by default except where the board can't have it or something it needs is off. Asking for a
# feature whose dependency is off is a configure error, never a silent drop (decision 2). The C code sees SOS_HAVE_<NAME> defined to
# 0 or 1 on every source of the target and tests it with #if (decision 4), and the image says
# which features it has (`version`, `health`, sos.features()) from SOS_FEATURE_LIST.
#
# Included after pico_sdk_init(): PICO_CYW43_SUPPORTED, which says whether the board has the
# Wi-Fi chip, exists only once the SDK has read the board header.

set(SOS_FEATURES "")

# sos_feature(<NAME> "<what it is>" [DEFAULT <ON|OFF>] [NEEDS <NAME>...])
# A feature defaults to ON, or to DEFAULT, but to OFF while something it needs is off: a plain
# Pico 2 (no Wi-Fi chip, so NET off) builds without TLS, HTTP, NTS and SSH unless told otherwise.
# An explicit ON with a need off is still an error (sos_features_apply()).
function(sos_feature name doc)
    cmake_parse_arguments(F "" "DEFAULT" "NEEDS" ${ARGN})
    if (NOT DEFINED F_DEFAULT)
        set(F_DEFAULT ON)
    endif()
    foreach(n ${F_NEEDS})
        if (NOT SOS_FEATURE_${n})
            set(F_DEFAULT OFF)
        endif()
    endforeach()
    option(SOS_FEATURE_${name} "${doc}" ${F_DEFAULT})
    set(SOS_FEATURE_NEEDS_${name} "${F_NEEDS}" PARENT_SCOPE)
    set(SOS_FEATURES ${SOS_FEATURES} ${name} PARENT_SCOPE)
endfunction()

# The Wi-Fi chip decides NET's default: a default that can't build is no default (§4.8).
if (PICO_CYW43_SUPPORTED)
    set(sos_net_default ON)
else()
    set(sos_net_default OFF)
endif()

sos_feature(NET  "Wi-Fi, lwIP, ping, SNTP" DEFAULT ${sos_net_default})
sos_feature(TLS  "TLS 1.3 client, the tls command" NEEDS NET)
sos_feature(HTTP "HTTP(S) client: http, fetch" NEEDS TLS)
sos_feature(NTS  "Authenticated time (NTS): nts, time server" NEEDS TLS)
sos_feature(SSH  "SSH server and SFTP: ssh, key" NEEDS NET)
sos_feature(LUA  "Lua scripts on core 1: lua")

# Check every rule, then define SOS_HAVE_<NAME> on <target> and SOS_FEATURE_LIST (the features
# line: what's in, then what's out).
function(sos_features_apply target)
    if (SOS_FEATURE_NET AND NOT PICO_CYW43_SUPPORTED)
        message(FATAL_ERROR "SOS_FEATURE_NET needs the Wi-Fi chip, and PICO_BOARD=${PICO_BOARD} "
            "has none: turn it off (-DSOS_FEATURE_NET=OFF), and what needs it")
    endif()
    set(have "")                        # not "on" and "off": if(off) is CMake's constant OFF
    set(lack "")
    foreach(f ${SOS_FEATURES})
        if (SOS_FEATURE_${f})
            foreach(n ${SOS_FEATURE_NEEDS_${f}})
                if (NOT SOS_FEATURE_${n})
                    message(FATAL_ERROR "SOS_FEATURE_${f} needs SOS_FEATURE_${n}: turn it off "
                        "too (-DSOS_FEATURE_${f}=OFF), or turn SOS_FEATURE_${n} on")
                endif()
            endforeach()
            target_compile_definitions(${target} PRIVATE SOS_HAVE_${f}=1)
            list(APPEND have ${f})
        else()
            target_compile_definitions(${target} PRIVATE SOS_HAVE_${f}=0)
            list(APPEND lack ${f})
        endif()
    endforeach()
    string(REPLACE ";" " " have "${have}")
    string(REPLACE ";" " " lack "${lack}")
    if (have)
        set(line "${have}")
    else()
        set(line "none")
    endif()
    if (lack)
        set(line "${line} (off: ${lack})")
    endif()
    target_compile_definitions(${target} PRIVATE "SOS_FEATURE_LIST=\"${line}\"")
    message(STATUS "SOS features: ${line}")
endfunction()
