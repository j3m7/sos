# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# A leaf check: the object file OBJ may reference no function or variable from outside except
# those in ALLOW (a ;-list). For code the SD card driver calls from inside its own paths
# (src/files/sdstat.c), which must count and return: no log, no card, no FatFs (sdstat.h).
#
#   cmake -DNM=<nm> -DOBJ=<file.o> -DALLOW=<a;b> -P leaf_check.cmake

execute_process(COMMAND ${NM} -u ${OBJ} OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if (NOT rc EQUAL 0)
    message(FATAL_ERROR "${NM} -u ${OBJ} failed")
endif()
string(REPLACE "\n" ";" lines "${out}")
set(bad "")
foreach(line ${lines})
    string(REGEX REPLACE "^[ \t]*U[ \t]+" "" sym "${line}")
    string(STRIP "${sym}" sym)
    string(REGEX REPLACE "^_" "" bare "${sym}")     # Mach-O's leading underscore
    if (sym STREQUAL "" OR bare IN_LIST ALLOW OR sym IN_LIST ALLOW)
        continue()
    endif()
    # The host build's instrumentation (tests/CMakeLists.txt turns on ASan and UBSan).
    if (bare MATCHES "^_*(asan|ubsan|sanitizer|lsan|tsan|msan)_")
        continue()
    endif()
    list(APPEND bad "${sym}")
endforeach()
if (bad)
    message(FATAL_ERROR "${OBJ} references ${bad}: code the SD driver calls from its own paths "
        "must count and return (src/files/sdstat.h)")
endif()
message(STATUS "${OBJ}: references only ${ALLOW}")
