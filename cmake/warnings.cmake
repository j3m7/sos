# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# Compiler warnings for SOS's own sources, shared by the firmware (CMakeLists.txt) and the host
# tests (tests/CMakeLists.txt), so both builds check the same code the same way. Third-party
# sources (Lua, wolfSSL, wolfSSH, the SD card driver, the copied pico-sshd) don't get them.
#
# Beyond -Wall -Wextra, the ones chosen are those that point at bugs, not style:
#   -Wconversion -Wsign-conversion  silent truncation and sign changes. On the 32-bit target
#                                   int, long and size_t are all 32 bits; the 64-bit host build
#                                   catches size_t -> int truncation the firmware build can't.
#   -Wshadow                        a declaration hiding another (a loop's n hiding a parameter n)
#   -Wformat=2                      printf formats that aren't literals, and %n
#   -Wcast-qual -Wwrite-strings     const dropped by a cast, or string literals written through
#   -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition
#                                   f() taking anything, and external functions without a
#                                   declaration in a header (should be static, or declared)
#   -Wredundant-decls               the same function declared twice
#   -Wundef                         #if on an undefined macro (a misspelled config switch is 0)
#   -Wvla                           variable-length arrays: unbounded stack use
#   -Wdouble-promotion              float arithmetic done in double; the Cortex-M33 FPU is
#                                   single precision, so double is software emulation
#   -Wcast-align=strict             casts to more aligned types, whatever the target allows
#   -Wnull-dereference -Wduplicated-cond -Wlogical-op -Wpointer-arith
#
# Left out: -Wformat-truncation=2 (assumes any caller-given size could be 1; level 1 is in
# -Wall), -Wswitch-default (style; a default hides -Wswitch's missing-enum warning) and
# -Wjump-misses-init (C++ compatibility; flags the usual goto-cleanup pattern).
set(SOS_WARNINGS
    -Wall -Wextra
    -Wconversion -Wsign-conversion
    -Wshadow
    -Wformat=2
    -Wcast-qual -Wwrite-strings
    -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition
    -Wredundant-decls
    -Wundef
    -Wvla
    -Wdouble-promotion
    -Wcast-align=strict
    -Wnull-dereference -Wduplicated-cond -Wlogical-op -Wpointer-arith
)
