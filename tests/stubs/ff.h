// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Host-test stand-in for FatFs's ff.h: the types, flags and functions src/files/files.c, tar.c
// and src/ssh/sftp_fs.c use, with the names and meanings FatFs gives them. The tests
// (tests/test_files.c, tests/test_sftp_fs.c, ...) implement the functions over a card in memory.
#pragma once

#include <stdint.h>

typedef unsigned int UINT;
typedef uint8_t BYTE;
typedef uint64_t FSIZE_t;

typedef enum {
    FR_OK = 0, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_NO_FILE, FR_NO_PATH, FR_INVALID_NAME,
    FR_DENIED, FR_EXIST, FR_INVALID_OBJECT, FR_WRITE_PROTECTED, FR_INVALID_DRIVE, FR_NOT_ENABLED,
    FR_NO_FILESYSTEM, FR_MKFS_ABORTED, FR_TIMEOUT, FR_LOCKED, FR_NOT_ENOUGH_CORE,
    FR_TOO_MANY_OPEN_FILES, FR_INVALID_PARAMETER
} FRESULT;

#define FA_READ          0x01
#define FA_WRITE         0x02
#define FA_CREATE_NEW    0x04
#define FA_CREATE_ALWAYS 0x08
#define FA_OPEN_ALWAYS   0x10
#define FA_OPEN_APPEND   0x30
#define AM_DIR           0x10
#define FF_FS_EXFAT      1          // 64-bit file offsets

typedef struct {
    int slot;                   // the test's file
    FSIZE_t pos;
    uint8_t mode;
} FIL;

typedef struct {
    int next;
} DIR;

typedef struct {
    FSIZE_t fsize;
    uint16_t fdate, ftime;
    uint8_t fattrib;
    char fname[256];
} FILINFO;

FSIZE_t test_f_size(const FIL *fp);
#define f_size(fp) test_f_size(fp)
#define f_tell(fp) ((fp)->pos)

FRESULT f_open(FIL *fp, const char *path, BYTE mode);
FRESULT f_close(FIL *fp);
FRESULT f_read(FIL *fp, void *buf, UINT btr, UINT *br);
FRESULT f_write(FIL *fp, const void *buf, UINT btw, UINT *bw);
FRESULT f_sync(FIL *fp);
FRESULT f_lseek(FIL *fp, FSIZE_t ofs);
FRESULT f_stat(const char *path, FILINFO *fno);
FRESULT f_opendir(DIR *dp, const char *path);
FRESULT f_readdir(DIR *dp, FILINFO *fno);
FRESULT f_closedir(DIR *dp);
FRESULT f_mkdir(const char *path);
FRESULT f_unlink(const char *path);
FRESULT f_rename(const char *path_old, const char *path_new);
FRESULT f_utime(const char *path, const FILINFO *fno);
