// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// File functions for wolfSSH's SFTP server (see src/sftp_wolf.c), on the SD card (FatFs).

#ifndef SOS_SFTP_FS_H
#define SOS_SFTP_FS_H

#include <stdint.h>

// `flags` are wolfSSH's WOLFSSH_O_* for FatFs (FA_* bits). Returns a descriptor, or -1.
int sftp_fs_open(const char *path, int flags);
// Read/write at `offset` (the SFTP request's: two words, low first, as wolfSSH's word32[2]);
// bytes done, or -1.
int sftp_fs_pread(int fd, unsigned char *buf, int len, const unsigned int *offset);
int sftp_fs_pwrite(int fd, const unsigned char *buf, int len, const unsigned int *offset);
int sftp_fs_close(int fd);
int sftp_fs_remove(const char *path);
int sftp_fs_rename(const char *from, const char *to);
int sftp_fs_mkdir(const char *path);
int sftp_fs_rmdir(const char *path);

#endif
