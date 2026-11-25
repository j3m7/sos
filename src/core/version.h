#pragma once

// The commit this firmware was built from: a 12-digit hash, "-dirty" appended when the tree had
// uncommitted changes, or "unknown" when it wasn't built from a git checkout. Generated at build
// time by cmake/version.cmake.
extern const char sos_version[];
