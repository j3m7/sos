// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Path handling for the file commands. Pure C (no FatFs), so it's unit tested on the host.

#ifndef SOS_PATH_H
#define SOS_PATH_H

#include <stdbool.h>
#include <stddef.h>

// Resolve `path` against the working directory `cwd` (absolute, e.g. "/" or "/docs") into an
// absolute, normalized path: "." and empty components dropped, ".." goes up (never above "/"),
// no trailing '/'. Returns false if the result doesn't fit in `out` (size `n`).
bool path_resolve(const char *cwd, const char *path, char *out, size_t n);

// The last component of a normalized path ("" for "/").
const char *path_basename(const char *path);

// Append "/<name>" to the directory `dir` (absolute, normalized) in `out`; false if too long.
bool path_join(const char *dir, const char *name, char *out, size_t n);

#endif
