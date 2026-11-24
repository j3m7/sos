// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// wolfSSH's SFTP server (src/wolfsftp.c in wolfSSH) with SOS's file functions.
//
// wolfSSH's FatFs port (wolfssh/port.h) maps file I/O to ff_open/ff_pread/ff_pwrite/ff_close,
// which ignore the request's file offset (any out-of-order or resumed transfer reads or writes
// the wrong place), keep a static pool of open files, and turn "create" into FA_CREATE_NEW
// (overwriting an existing file fails); its remove and rename return FatFs codes, which the
// callers read as success when they fail. port.h has include guards, so including it first and
// then redefining those macros makes wolfsftp.c use src/ssh/sftp_fs.c instead, without patching
// wolfSSH. Every change also passes files_write_allowed() (src/files/files.h).

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssh/ssh.h>
#include <wolfssh/internal.h>
#include <wolfssh/wolfsftp.h>

#include "ssh/sftp_fs.h"

#undef WOPEN
#undef WPREAD
#undef WPWRITE
#undef WCLOSE
#undef WREMOVE
#undef WRENAME
#undef WMKDIR
#undef WRMDIR
#define WOPEN(fs, f, m, p)          sftp_fs_open((f), (m))
#define WPREAD(fs, fd, b, s, o)     sftp_fs_pread((fd), (b), (s), (o))
#define WPWRITE(fs, fd, b, s, o)    sftp_fs_pwrite((fd), (b), (s), (o))
#define WCLOSE(fs, fd)              sftp_fs_close((fd))
// Changes go through files_write_allowed() (src/files/files.h) and report failures as wolfsftp.c
// expects (the port returned FatFs codes, > 0 on failure, where it tests for < 0).
#define WREMOVE(fs, p)              sftp_fs_remove((p))
#define WRENAME(fs, o, n)           sftp_fs_rename((o), (n))
#define WMKDIR(fs, p, m)            sftp_fs_mkdir((p))
#define WRMDIR(fs, p)               sftp_fs_rmdir((p))
// Attribute changes (setstat: chmod, times) aren't compiled for FatFs in wolfsftp.c.

#ifndef PRINTF
#define PRINTF(...) ((void)0)       // the port's unused ff_open() traces with it
#endif

#include "src/wolfsftp.c"
