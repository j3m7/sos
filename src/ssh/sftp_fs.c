// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// File functions for wolfSSH's SFTP server on the SD card. Compared with wolfSSH's FatFs port:
// reads and writes go to the requested offset, "create" doesn't fail on an existing file, open
// files are allocated as needed (and count as busy for `sd format`/`sd unmount`), and a file being
// written is synced about once a second (files_sync_if_due()), so `ls` shows an upload's progress
// and a reset loses at most the last second of it.

#include "ssh/sftp_fs.h"

#include <stdlib.h>

#include "pico/stdlib.h"

#include "ff.h"

#include "core/heaptag.h"
#include "files/files.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssh/error.h>              // WS_PERMISSIONS

#define SFTP_MAX_OPEN 8         // across all SFTP sessions (FatFs allows FF_FS_LOCK = 16 in all)

static FIL *open_files[SFTP_MAX_OPEN];
// When each handle was opened or last synced (files_sync_if_due()). Set at every open: a
// read-only handle never reaches the sync (its f_write() fails), so setting it costs nothing but
// spares open() a branch. A parallel array keeps the heap blocks FatFs-sized.
static uint64_t synced_at[SFTP_MAX_OPEN];

// wolfSSH passes FA_* bits built from the SFTP open flags: READ/WRITE, CREAT = FA_CREATE_NEW |
// FA_OPEN_ALWAYS, TRUNC = FA_CREATE_ALWAYS, APPEND = FA_OPEN_APPEND. Turn them into one FatFs
// open mode: FA_CREATE_NEW on its own would fail for a file that exists.
static BYTE fatfs_mode(int flags) {
    BYTE mode = (BYTE)(flags & (FA_READ | FA_WRITE));
    if ((flags & FA_OPEN_APPEND) == FA_OPEN_APPEND)
        mode |= FA_OPEN_APPEND;
    else if (flags & FA_CREATE_ALWAYS)
        mode |= FA_CREATE_ALWAYS;   // truncate (and create)
    else if (flags & FA_OPEN_ALWAYS)
        mode |= FA_OPEN_ALWAYS;     // create if missing
    return mode;
}

int sftp_fs_open(const char *path, int flags) {
    if (!(flags & (FA_READ | FA_WRITE)))
        return -1;
    if ((flags & (FA_WRITE | FA_CREATE_ALWAYS | FA_OPEN_ALWAYS | FA_CREATE_NEW)) &&
        !files_write_allowed(NULL, path))
        return -1;
    for (int fd = 0; fd < SFTP_MAX_OPEN; fd++) {
        if (open_files[fd])
            continue;
        FIL *f = heap_alloc(HEAP_NET, sizeof(FIL));
        if (!f)
            return -1;
        if (f_open(f, path, fatfs_mode(flags)) != FR_OK) {
            heap_release(HEAP_NET, f);
            return -1;
        }
        open_files[fd] = f;
        synced_at[fd] = time_us_64();
        files_hold();
        return fd;
    }
    return -1;                      // too many open files
}

static FIL *get(int fd) {
    return (fd >= 0 && fd < SFTP_MAX_OPEN) ? open_files[fd] : NULL;
}

// SFTP offsets are 64-bit: ofst[0] is the low word, ofst[1] the high one.
static bool seek(FIL *f, const unsigned int *ofst) {
    FSIZE_t pos = (FSIZE_t)ofst[0];
#if FF_FS_EXFAT
    pos |= (FSIZE_t)ofst[1] << 32;
#else
    if (ofst[1])
        return false;
#endif
    return f_tell(f) == pos || f_lseek(f, pos) == FR_OK;
}

int sftp_fs_pread(int fd, unsigned char *buf, int len, const unsigned int *offset) {
    FIL *f = get(fd);
    UINT n;
    if (!f || len < 0 || !seek(f, offset) || f_read(f, buf, (UINT)len, &n) != FR_OK)
        return -1;
    return (int)n;
}

int sftp_fs_pwrite(int fd, const unsigned char *buf, int len, const unsigned int *offset) {
    FIL *f = get(fd);
    UINT n;
    // Seeking past the end extends the file (FatFs, in write mode), as pwrite() does.
    // Then a sync if the last one is a second old; its failure is the write's. wolfsftp.c calls
    // this once per SFTP write request (again only after a short write), never with len 0.
    if (!f || len < 0 || !seek(f, offset) || f_write(f, buf, (UINT)len, &n) != FR_OK ||
        files_sync_if_due(f, &synced_at[fd]) != FR_OK)
        return -1;
    return (int)n;
}

// The rest return what the callers in wolfsftp.c test for: remove and rename fail on < 0;
// mkdir and rmdir on != 0, with WS_PERMISSIONS reported to the client as "permission denied".
// (wolfSSH's FatFs port returned FatFs codes, which are > 0 on failure, so a failed remove or
// rename was reported as done.)

int sftp_fs_remove(const char *path) {
    if (!files_write_allowed(NULL, path))
        return -1;
    return f_unlink(path) == FR_OK ? 0 : -1;
}

int sftp_fs_rename(const char *from, const char *to) {
    if (!files_write_allowed(NULL, from) || !files_write_allowed(NULL, to))
        return -1;
    return f_rename(from, to) == FR_OK ? 0 : -1;
}

int sftp_fs_mkdir(const char *path) {
    if (!files_write_allowed(NULL, path))
        return WS_PERMISSIONS;
    return f_mkdir(path) == FR_OK ? 0 : -1;
}

int sftp_fs_rmdir(const char *path) {
    if (!files_write_allowed(NULL, path))
        return WS_PERMISSIONS;
    return f_unlink(path) == FR_OK ? 0 : -1;
}

int sftp_fs_close(int fd) {
    FIL *f = get(fd);
    if (!f)
        return -1;
    FRESULT fr = f_close(f);
    heap_release(HEAP_NET, f);
    open_files[fd] = NULL;
    files_release();
    return fr == FR_OK ? 0 : -1;
}
