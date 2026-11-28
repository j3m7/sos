// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// FatFs's configuration for SOS: the SD card library's (lib/sdcard/src/include/ffconf.h) with
// one change, FF_USE_CHMOD 1, so f_utime() exists and `tar xf` can give files the archive's
// dates (docs/design/archives.md §6.5).
//
// ff.h includes "ffconf.h" with quotes and has none beside it, so the include path decides
// which one is found. This directory comes before the library's on the path of everything that
// includes ff.h (CMakeLists.txt, the no-OS-FatFS target, whose sources ff.c among them are
// compiled in the sos target): one definition, so FatFs and SOS agree on FIL and FILINFO.
// #include_next takes the library's file itself, so its other settings follow its updates.

#include_next "ffconf.h"

#undef FF_USE_CHMOD
#define FF_USE_CHMOD 1
