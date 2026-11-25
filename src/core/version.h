#pragma once

// The commit this firmware was built from: a 12-digit hash, "-dirty" appended when the tree had
// uncommitted changes, or "unknown" when it wasn't built from a git checkout. Generated at build
// time by cmake/version.cmake.
//
// It names the SOS commit only. Libraries taken from outside the tree (WOLFSSL_ROOT, WOLFSSH_ROOT,
// SDCARD_ROOT, LUA_ROOT in local.cmake) aren't covered, so two images with the same clean hash
// can still differ in those libraries.
extern const char sos_version[];
