// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// sos_version for tests that link shell_core.c (its greeting prints it). The firmware's is
// generated at build time by cmake/version.cmake.

#include "core/version.h"

const char sos_version[] = "0123456789ab";
